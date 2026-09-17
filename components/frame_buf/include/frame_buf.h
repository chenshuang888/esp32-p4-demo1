#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct frame_buf_s frame_buf_t;

typedef struct {
    uint32_t slot_size;                          /* 每槽字节数 */
    bool     has_len;                            /* 是否记录每帧实际长度 */
    void    *(*alloc)(size_t size, size_t *actual); /* NULL -> 默认 PSRAM 64B 对齐分配 */
} frame_buf_cfg_t;

frame_buf_t *frame_buf_create(const frame_buf_cfg_t *cfg);
void frame_buf_delete(frame_buf_t *fb);

/* 生产者（非阻塞）：返回当前写槽，永不等待 */
uint8_t *frame_buf_get_write(frame_buf_t *fb);

/* 生产者：尝试发布本帧。
 * true  : 消费者已释放读槽，本帧发布（swap 索引 + 唤醒消费者）
 * false : 消费者未释放读槽，本帧丢弃，继续在写槽上覆盖 */
bool frame_buf_commit(frame_buf_t *fb, uint32_t len);

/* 消费者：阻塞等新帧（timeout_ms=0 即轮询，不阻塞） */
int frame_buf_wait_new(frame_buf_t *fb, uint32_t timeout_ms);

/* 消费者：取读槽并标记占用（read_done 前生产者不覆盖该槽）。
 * out_len 可空；仅 has_len=true 时填充 */
uint8_t *frame_buf_get_read(frame_buf_t *fb, uint32_t *out_len);

/* 消费者：读完成，释放读槽 */
void frame_buf_read_done(frame_buf_t *fb);
