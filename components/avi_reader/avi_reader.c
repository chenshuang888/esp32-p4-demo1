/*
 * avi_reader —— MJPEG-in-AVI 解封装（实现）
 *
 * ===================== 它认的文件长什么样 =====================
 * 就是 avi_writer 写出来的那种（无 idx1 索引）：
 *
 *     RIFF____AVI
 *       LIST____hdrl
 *         avih  ← 我们要的：微秒/帧、宽高、总帧数
 *         LIST____strl (strh + strf)   ← 跳过
 *       LIST____movi
 *         00dc ____ <JPEG>            ← 一帧一个块，顺序排列
 *         00dc ____ <JPEG>
 *         ...
 *
 * 所以实现就是：扫顶层块找到 movi，记下它的起止偏移；之后 next() 在 movi 范围内
 * 顺序读块，遇到 00dc 就是帧，其它块跳过。RIFF 要求块内容是偶数长度，奇数补 1 字节
 * —— **这个填充必须跳**，否则下一块的 fourcc 就错位了。
 *
 * ===================== 两块缓冲都必须 128B 对齐 =====================
 * ESP32-P4 的 SD 盘层（sdmmc_read_sectors）会在源缓冲不满足 DMA 对齐时**静默**把
 * 一次多块读拆成"逐个 512B 单块读"，速度掉约 20 倍。所以这里两块缓冲都用
 * heap_caps_aligned_alloc(128, ...)：
 *   frame → 帧数据落点（也是交给调用方的指针，所以本身必须是独立的连续块）
 *   vbuf  → stdio 的读缓冲（头部的 8 字节小块读也走它，保证盘层看到的还是对齐缓冲）
 * 长度方面 80KB 和 4KB 都是 512 的整数倍，满足要求。
 * ==============================================================
 */
#include "avi_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "avi_reader";

/* 和上面的对齐说明配套 */
#define AVI_DMA_ALIGN   128
#define AVI_VBUF_BYTES  4096      /* stdio 读缓冲（4KB，也是 512 的整数倍） */

#define AVI_AVIH_SIZE   56        /* avih 块的固定内容长度 */
#define AVI_AVIH_USEC_OFF        0    /* dwMicroSecPerFrame */
#define AVI_AVIH_TOTALFRAMES_OFF 16
#define AVI_AVIH_WIDTH_OFF       32
#define AVI_AVIH_HEIGHT_OFF      36

struct avi_reader_s {
    FILE      *f;
    uint8_t   *frame;       /* 128B 对齐的帧缓冲（AVI_READER_FRAME_MAX 字节） */
    uint8_t   *vbuf;        /* 128B 对齐的 stdio 读缓冲 */
    avi_info_t info;
    bool       have_info;   /* 是否解析到了 avih */
    long       movi_start;  /* movi 负载起始偏移 */
    long       movi_end;    /* movi 负载结束偏移（不含） */
    bool       started;     /* 是否已经定位到 movi 负载开头 */
    uint32_t   frames;      /* 已读出的帧数（诊断用） */
};

/* ---------------- 小工具 ---------------- */

static bool rd(FILE *f, void *dst, size_t n)
{
    return fread(dst, 1, n, f) == n;
}

static uint32_t u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------------- 在 hdrl 里找 avih ---------------- */
static void parse_hdrl(avi_reader_t *r, long end)
{
    while (ftell(r->f) + 8 <= end) {
        uint8_t ch[8];
        if (!rd(r->f, ch, sizeof(ch))) {
            return;
        }
        const uint32_t size = u32le(ch + 4);
        const long body = ftell(r->f);

        if (memcmp(ch, "avih", 4) == 0 && size >= 40) {
            uint8_t a[AVI_AVIH_SIZE];
            const size_t n = (size < sizeof(a)) ? size : sizeof(a);
            if (!rd(r->f, a, n)) {
                return;
            }
            const uint32_t us = u32le(a + AVI_AVIH_USEC_OFF);
            /* fps = 1e6 / 微秒每帧，四舍五入（例如 71429us -> 14fps） */
            r->info.fps = us ? (1000000u + us / 2u) / us : 0;
            r->info.frame_count = u32le(a + AVI_AVIH_TOTALFRAMES_OFF);
            r->info.width  = (uint16_t)u32le(a + AVI_AVIH_WIDTH_OFF);
            r->info.height = (uint16_t)u32le(a + AVI_AVIH_HEIGHT_OFF);
            r->have_info = true;
        }

        /* 下一个块：跳过内容 + 奇数填充 */
        if (fseek(r->f, body + (long)size + (long)(size & 1u), SEEK_SET) != 0) {
            return;
        }
    }
}

/* ---------------- 扫顶层块，定位 movi ---------------- */
static bool avi_parse(avi_reader_t *r)
{
    uint8_t hdr[12];
    if (!rd(r->f, hdr, sizeof(hdr))) {
        ESP_LOGE(TAG, "short file");
        return false;
    }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "AVI ", 4) != 0) {
        ESP_LOGE(TAG, "not a RIFF/AVI file");
        return false;
    }
    const long file_size = 8 + (long)u32le(hdr + 4);   /* RIFF size 是"文件大小 - 8" */

    for (;;) {
        const long pos = ftell(r->f);
        if (pos < 0 || pos + 8 > file_size) {
            break;
        }

        uint8_t ch[8];
        if (!rd(r->f, ch, sizeof(ch))) {
            break;
        }
        const uint32_t size = u32le(ch + 4);

        if (memcmp(ch, "LIST", 4) == 0) {
            uint8_t type[4];
            if (!rd(r->f, type, sizeof(type))) {
                break;
            }
            const long payload  = ftell(r->f);          /* 'LIST' 的内容（含 type 四字节）起点之后 */
            const long list_end = payload + (long)size - 4;
            if (list_end <= payload) {
                break;                                  /* 大小非法 */
            }

            if (memcmp(type, "movi", 4) == 0) {
                /* movi 找到了。avih 在它前面，此时应该已经解析过 */
                r->movi_start = payload;
                r->movi_end   = list_end;
                return true;
            }
            if (memcmp(type, "hdrl", 4) == 0) {
                parse_hdrl(r, list_end);
            }
            if (fseek(r->f, list_end, SEEK_SET) != 0) {
                break;
            }
        } else {
            /* 普通块：跳过内容 + 填充 */
            if (fseek(r->f, (long)size + (long)(size & 1u), SEEK_CUR) != 0) {
                break;
            }
        }
    }

    ESP_LOGE(TAG, "movi chunk not found");
    return false;
}

/* =====================================================================
 * 对外接口
 * ===================================================================== */

avi_reader_t *avi_reader_open(const char *path)
{
    if (path == NULL) {
        return NULL;
    }

    avi_reader_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->f = fopen(path, "rb");
    if (r->f == NULL) {
        ESP_LOGE(TAG, "fopen(%s) failed", path);
        free(r);
        return NULL;
    }

    /* 两块缓冲都必须 128B 对齐（理由见文件顶部） */
    r->frame = heap_caps_aligned_alloc(AVI_DMA_ALIGN, AVI_READER_FRAME_MAX, MALLOC_CAP_SPIRAM);
    r->vbuf  = heap_caps_aligned_alloc(AVI_DMA_ALIGN, AVI_VBUF_BYTES, MALLOC_CAP_SPIRAM);
    if (r->frame == NULL || r->vbuf == NULL) {
        ESP_LOGE(TAG, "no mem for reader buffers");
        avi_reader_close(r);
        return NULL;
    }
    /* setvbuf 必须在任何 I/O 之前 —— 下面 avi_parse 才开始读 */
    setvbuf(r->f, (char *)r->vbuf, _IOFBF, AVI_VBUF_BYTES);

    if (!avi_parse(r)) {
        avi_reader_close(r);
        return NULL;
    }

    ESP_LOGI(TAG, "open %s: %ux%u @%ufps, %u frames",
             path, (unsigned)r->info.width, (unsigned)r->info.height,
             (unsigned)r->info.fps, (unsigned)r->info.frame_count);
    return r;
}

bool avi_reader_get_info(const avi_reader_t *r, avi_info_t *out)
{
    if (r == NULL || out == NULL) {
        return false;
    }
    *out = r->info;
    return r->have_info;
}

esp_err_t avi_reader_next(avi_reader_t *r, const uint8_t **jpg, size_t *len)
{
    if (r == NULL || jpg == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!r->started) {
        if (fseek(r->f, r->movi_start, SEEK_SET) != 0) {
            return ESP_FAIL;
        }
        r->started = true;
    }

    while (ftell(r->f) + 8 <= r->movi_end) {
        uint8_t ch[8];
        if (!rd(r->f, ch, sizeof(ch))) {
            break;
        }
        const uint32_t size = u32le(ch + 4);
        const long body = ftell(r->f);
        if (body + (long)size > r->movi_end) {
            ESP_LOGW(TAG, "chunk overruns movi, stop");
            break;
        }

        if (memcmp(ch, "00dc", 4) == 0) {
            if (size == 0 || size > AVI_READER_FRAME_MAX) {
                ESP_LOGE(TAG, "frame %u has bad size %u", (unsigned)r->frames, (unsigned)size);
                return ESP_ERR_INVALID_SIZE;
            }
            if (!rd(r->f, r->frame, size)) {
                return ESP_FAIL;
            }
            *jpg = r->frame;
            *len = size;
            r->frames++;
            if (size & 1u) {
                fseek(r->f, 1, SEEK_CUR);       /* 跳过填充，为下一块的 fourcc 定位 */
            }
            return ESP_OK;
        }

        /* 不是视频帧：跳过（含填充） */
        if (fseek(r->f, (long)size + (long)(size & 1u), SEEK_CUR) != 0) {
            break;
        }
    }

    return ESP_ERR_NOT_FOUND;                   /* 正常播完 */
}

void avi_reader_close(avi_reader_t *r)
{
    if (r == NULL) {
        return;
    }
    if (r->f != NULL) {
        fclose(r->f);
    }
    if (r->frame != NULL) {
        heap_caps_free(r->frame);
    }
    if (r->vbuf != NULL) {
        heap_caps_free(r->vbuf);
    }
    free(r);
}
