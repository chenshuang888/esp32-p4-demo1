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

/* 默认分配器：PSRAM，按 **L2 cache 行大小** 对齐。
 *
 * ⚠️ 这个对齐是**硬要求**，不是调优：
 *   槽可能要交给 DMA 写（典型是 JPEG 解码器的 RGB565 输出，2DDMA 直接写 PSRAM），
 *   之后还要 esp_cache_msync() 按 cache 行做失效 —— 所以**地址和长度**都必须是
 *   cache 行的整数倍。IDF 的 esp_dma_is_buffer_alignment_satisfied() 检查的正是
 *   lcm(dma_align=4, cache_line)，不满足时解码器直接返回 ESP_ERR_INVALID_ARG：
 *       "jpeg decode decode_outbuf or out_buffer size is not aligned"
 *
 * ⚠️ 所以**不能写死 64** —— demo1 就是 64，因为它的 L2 cache line 是 64B；
 *    本项目是 CONFIG_CACHE_L2_CACHE_LINE_SIZE = 128（厂家配置）。这个差异正是
 *    "demo1 能跑、这里跑不了"的全部原因。
 *
 * 这里直接取 128 而不做动态查询，是因为 ESP32-P4 的 L2 cache line **只有 64B / 128B
 * 两档**（见 esp_system/port/soc/esp32p4/Kconfig.cache），128 是上限，取它一定安全。
 * （能动态查的 esp_cache_get_alignment() 是**私有接口**，声明在
 *  esp_private/esp_cache_private.h 里，不适合组件使用。）
 *
 * 释放统一用 heap_caps_free，注入的 allocator 须为 heap_caps 系 */
#define FB_PSRAM_ALIGN  128

static void *fb_default_alloc(size_t size, size_t *actual)
{
    /* 长度也向上对齐（IDF 对输出缓冲的要求）。实际分配可能略大于 slot_size ——
     * 对生产者/消费者都无害，只是拿到的缓冲比声明的大一点 */
    const size_t aligned = (size + FB_PSRAM_ALIGN - 1) / FB_PSRAM_ALIGN * FB_PSRAM_ALIGN;
    void *p = heap_caps_aligned_alloc(FB_PSRAM_ALIGN, aligned, MALLOC_CAP_SPIRAM);
    if (p != NULL && actual != NULL) {
        *actual = aligned;
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
