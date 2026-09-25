/*
 * avi_writer —— 把一串 JPEG 帧写成 MJPEG-in-AVI 文件
 *
 * 定位：**一个同步的格式编码器**，不是一条管线。没有任务、没有锁、没有队列。
 *   - 输入是"一帧 JPEG 的裸指针 + 长度"，输出是一个 .avi 文件（走到 close 才收尾）；
 *   - 它不知道帧从哪来（USB 相机 / 相册 / SD 文件都行），也不认识 frame_buf、相机、LVGL；
 *   - 内部用 newlib 的 FILE*，所以调用方只需给一个路径，不依赖 VFS/FATFS 的任何细节。
 *
 * 用法：
 *     avi_writer_t *aw = avi_writer_open("/sdcard/DCIM/VID_0001.avi", 640, 480, 15);
 *     ...
 *     avi_writer_add_frame(aw, jpg, len);      // 每来一帧调一次
 *     ...
 *     uint32_t frames = 0;
 *     avi_writer_close(aw, &frames);           // 回填头 + fclose，之后句柄失效
 *
 * ===================== 为什么不写 idx1 索引 =====================
 * AVI 的索引块（idx1）是可选的。不写它的代价是"某些播放器拖进度条时不准"，
 * 换来的是**不必在内存里累积每帧偏移**（一小时的 15fps 视频要 5 万条记录）。
 * VLC / ffmpeg 这类播放器会自己扫 movi 重建索引，所以本版先省略。
 * 以后要做精确 seek 再加 —— 那时在 close 里追加 idx1 即可，填充逻辑是自包含的。
 * ==============================================================
 *
 * ===================== 为什么"头写完不回填到最后一刻" =====================
 * AVI 头里的 RIFF size / movi size / dwTotalFrames 三个字段在写第一帧时还不知道值，
 * 所以 open 时写占位 0，close 时用 fseek 回去填。三个位置在 open 里就记下来了。
 * 注意这几个位置都在文件开头（< 256 字节），而且 close 里算用的是**累加出的字节数**
 * 而不是 ftell —— ESP32 上 long 是 32 位，录到 2GB 以上 ftell 会溢出。
 * ======================================================================
 */
#include "avi_writer.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     /* fsync, fileno */

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "avi_writer";

/* =====================================================================
 * stdio 写缓冲 —— **必须 128 字节对齐，这是录像能吃满 SD 写速的关键，别删也别改对齐**
 *
 * 原因在 SD 盘层：sdmmc_write_sectors()（IDF 的 sdmmc_cmd.c:448）会先检查源缓冲
 * 是否满足 DMA 对齐要求，**不满足就静默降级成"一次只写一个 512 字节块"**
 * （逐个 memcpy 到临时缓冲再单块传输）。单块写每块都要一次完整命令 + 等卡忙，
 * 在这张卡上约 4ms/块 → 整条录像链路掉到 ~120KB/s（实测）。
 *
 * ESP32-P4 的 L2 cache line 是 128B，SDMMC 走 PSRAM DMA 时要求缓冲按 128B 对齐
 * （长度是块大小的整数倍即可，512 的倍数自然满足）。
 *
 * 旁证：拍照为什么一直很快 —— 它的源缓冲是 frame_buf 的槽，而 frame_buf.c 用的是
 * heap_caps_aligned_alloc(128, ...)。对齐了 → 多块连续写 → 快。
 *
 * 128KB 同时也是"攒够一大块再刷"的粒度（是 512 的整数倍）。
 * ===================================================================== */
#define AVI_WBUF_BYTES   (128 * 1024)
#define AVI_WBUF_ALIGN   128

/* =====================================================================
 * AVI 结构里用到的固定字段值
 *
 * AVI = RIFF 容器。层级是：RIFF(整文件) > LIST hdrl(头) > LIST strl(流描述)，
 * 再往后跟 LIST movi（真正的一帧帧数据）。每个块都是 "4字节fourcc + 4字节大小 +
 * 内容"，大小只算内容、不算自己那 8 字节头，内容长度是奇数时补 1 个 0 对齐。
 * ===================================================================== */

/* hdrl LIST 的内容大小 = 'hdrl'(4) + avih块(8+56) + strl的LIST块(8 + (4 + 64 + 48)) */
#define AVI_HDRL_SIZE  192
/* strl LIST 的内容大小 = 'strl'(4) + strh块(8+56) + strf块(8+40) */
#define AVI_STRL_SIZE  116
#define AVI_AVIH_SIZE  56
#define AVI_STRH_SIZE  56
#define AVI_STRF_SIZE  40      /* = BITMAPINFOHEADER */
#define AVI_MJPG_TAG   "MJPG"

struct avi_writer_s {
    FILE    *f;
    uint8_t *wbuf;          /* setvbuf 用的写缓冲（PSRAM），fclose 后释放 */
    uint16_t w;
    uint16_t h;
    uint32_t fps;
    uint32_t frames;        /* 已写入的帧数（回填 dwTotalFrames / 返回给调用方） */
    uint32_t movi_bytes;    /* 'movi' 之后累积写入的字节数（含量各帧的 chunk 头与对齐填充） */
    long     hdr_end;       /* 头写完时的文件偏移 = movi 负载起点（close 里算 riff size 用） */
    long     pos_riff_size; /* RIFF size 字段的位置 */
    long     pos_avih_frames;/* avih.dwTotalFrames 字段的位置 */
    long     pos_movi_size; /* movi LIST 的 size 字段位置 */
    bool     ok;            /* 写帧过程中出过错，close 返回 ESP_FAIL */
};

/* ---------------- 小工具：按小端写定长整数 / fourcc ----------------
 * 头里的写失败不逐个判 —— 统一在 open 末尾用 ferror() 检查一次。 */

static void wr_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static void wr_u16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

static void wr_fourcc(FILE *f, const char *s)
{
    fwrite(s, 1, 4, f);
}

/* 回填一个 u32 到文件里已记录的位置，完事把文件指针还回去 */
static void patch_u32(FILE *f, long pos, uint32_t v)
{
    const long cur = ftell(f);
    if (fseek(f, pos, SEEK_SET) == 0) {
        wr_u32(f, v);
    }
    fseek(f, cur, SEEK_SET);
}

/* =====================================================================
 * open：写完整套头（三个未知字段先写 0），打开 movi
 * ===================================================================== */
avi_writer_t *avi_writer_open(const char *path, uint16_t w, uint16_t h, uint32_t fps)
{
    if (path == NULL || w == 0 || h == 0 || fps == 0) {
        return NULL;
    }

    avi_writer_t *aw = calloc(1, sizeof(*aw));
    if (aw == NULL) {
        return NULL;
    }
    aw->f   = fopen(path, "wb");
    aw->w   = w;
    aw->h   = h;
    aw->fps = fps;
    aw->ok  = true;
    if (aw->f == NULL) {
        free(aw);
        return NULL;
    }

    /* ⚠️ 在任何 I/O 之前给 stdio 换上**128B 对齐**的大缓冲（理由见文件顶部 AVI_WBUF_BYTES）。
     * 这是"录像能写满 SD 写速"的关键：不对齐的话 SD 盘层会把每次刷盘拆成 512B 单块写，
     * 速度掉到 ~1/20。分配失败不致命，只是退回默认缓冲（慢）。 */
    aw->wbuf = heap_caps_aligned_alloc(AVI_WBUF_ALIGN, AVI_WBUF_BYTES, MALLOC_CAP_SPIRAM);
    if (aw->wbuf != NULL) {
        if (setvbuf(aw->f, (char *)aw->wbuf, _IOFBF, AVI_WBUF_BYTES) != 0) {
            heap_caps_free(aw->wbuf);
            aw->wbuf = NULL;
        }
    }
    if (aw->wbuf == NULL) {
        ESP_LOGW(TAG, "no aligned %dKB write buffer, SD write will fall back to slow 512B blocks",
                 AVI_WBUF_BYTES / 1024);
    }

    /* --- RIFF 头 --- */
    wr_fourcc(aw->f, "RIFF");
    aw->pos_riff_size = ftell(aw->f);
    wr_u32(aw->f, 0);                      /* 占位：整文件大小 - 8，close 回填 */
    wr_fourcc(aw->f, "AVI ");

    /* --- LIST hdrl --- */
    wr_fourcc(aw->f, "LIST");
    wr_u32(aw->f, AVI_HDRL_SIZE);
    wr_fourcc(aw->f, "hdrl");

    /* avih：主头。fps 用"微秒/帧"表达，播放器据此定时 */
    wr_fourcc(aw->f, "avih");
    wr_u32(aw->f, AVI_AVIH_SIZE);
    wr_u32(aw->f, 1000000u / fps);         /* dwMicroSecPerFrame */
    wr_u32(aw->f, 0);                      /* dwMaxBytesPerSec（无关紧要） */
    wr_u32(aw->f, 0);                      /* dwPaddingGranularity */
    wr_u32(aw->f, 0);                      /* dwFlags：没有索引，所以不含 AVIF_HASINDEX */
    aw->pos_avih_frames = ftell(aw->f);
    wr_u32(aw->f, 0);                      /* dwTotalFrames：占位，close 回填 */
    wr_u32(aw->f, 0);                      /* dwInitialFrames */
    wr_u32(aw->f, 1);                      /* dwStreams */
    wr_u32(aw->f, 0);                      /* dwSuggestedBufferSize */
    wr_u32(aw->f, w);                      /* dwWidth */
    wr_u32(aw->f, h);                      /* dwHeight */
    wr_u32(aw->f, 0);                      /* dwReserved[4] */
    wr_u32(aw->f, 0);
    wr_u32(aw->f, 0);
    wr_u32(aw->f, 0);

    /* --- LIST strl（一个视频流） --- */
    wr_fourcc(aw->f, "LIST");
    wr_u32(aw->f, AVI_STRL_SIZE);
    wr_fourcc(aw->f, "strl");

    /* strh：流头。fps = dwRate / dwScale */
    wr_fourcc(aw->f, "strh");
    wr_u32(aw->f, AVI_STRH_SIZE);
    wr_fourcc(aw->f, "vids");              /* fccType：视频流 */
    wr_fourcc(aw->f, AVI_MJPG_TAG);        /* fccHandler：本流由 MJPEG 压缩 */
    wr_u32(aw->f, 0);                      /* dwFlags */
    wr_u16(aw->f, 0);                      /* wPriority */
    wr_u16(aw->f, 0);                      /* wLanguage */
    wr_u32(aw->f, 0);                      /* dwInitialFrames */
    wr_u32(aw->f, 1);                      /* dwScale */
    wr_u32(aw->f, fps);                    /* dwRate */
    wr_u32(aw->f, 0);                      /* dwStart */
    wr_u32(aw->f, 0);                      /* dwLength（帧数，可选；不填也不影响播放） */
    wr_u32(aw->f, 0);                      /* dwSuggestedBufferSize */
    wr_u32(aw->f, 0xFFFFFFFFu);            /* dwQuality：-1 = 默认 */
    wr_u32(aw->f, 0);                      /* dwSampleSize */
    wr_u16(aw->f, 0);                      /* rcFrame.left */
    wr_u16(aw->f, 0);                      /* rcFrame.top */
    wr_u16(aw->f, w);                      /* rcFrame.right */
    wr_u16(aw->f, h);                      /* rcFrame.bottom */

    /* strf：BITMAPINFOHEADER。MJPEG 的约定是 biBitCount=24、biCompression='MJPG' */
    wr_fourcc(aw->f, "strf");
    wr_u32(aw->f, AVI_STRF_SIZE);
    wr_u32(aw->f, AVI_STRF_SIZE);          /* biSize */
    wr_u32(aw->f, w);                      /* biWidth */
    wr_u32(aw->f, h);                      /* biHeight */
    wr_u16(aw->f, 1);                      /* biPlanes */
    wr_u16(aw->f, 24);                     /* biBitCount */
    wr_fourcc(aw->f, AVI_MJPG_TAG);        /* biCompression */
    wr_u32(aw->f, (uint32_t)w * h * 3);    /* biSizeImage */
    wr_u32(aw->f, 0);                      /* biXPelsPerMeter */
    wr_u32(aw->f, 0);                      /* biYPelsPerMeter */
    wr_u32(aw->f, 0);                      /* biClrUsed */
    wr_u32(aw->f, 0);                      /* biClrImportant */

    /* --- LIST movi：帧数据从这里开始，大小 close 时回填 --- */
    wr_fourcc(aw->f, "LIST");
    aw->pos_movi_size = ftell(aw->f);
    wr_u32(aw->f, 0);                      /* 占位：close 回填 */
    wr_fourcc(aw->f, "movi");
    aw->hdr_end = ftell(aw->f);            /* movi 负载起点 */

    if (ferror(aw->f)) {
        ESP_LOGE(TAG, "header write failed for %s", path);
        fclose(aw->f);
        if (aw->wbuf != NULL) {
            heap_caps_free(aw->wbuf);
        }
        free(aw);
        return NULL;
    }
    ESP_LOGI(TAG, "open %s (%ux%u @%ufps)", path, (unsigned)w, (unsigned)h, (unsigned)fps);
    return aw;
}

/* =====================================================================
 * add_frame：append 一个 '00dc' 块
 * ===================================================================== */
esp_err_t avi_writer_add_frame(avi_writer_t *aw, const uint8_t *jpg, size_t len)
{
    if (aw == NULL || jpg == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!aw->ok) {
        return ESP_FAIL;
    }

    /* 块头：fourcc '00dc'（未压缩/独立帧的视频块）+ 4 字节长度 */
    uint8_t hdr[8] = { '0', '0', 'd', 'c',
                       (uint8_t)len, (uint8_t)(len >> 8),
                       (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    if (fwrite(hdr, 1, sizeof(hdr), aw->f) != sizeof(hdr) ||
        fwrite(jpg, 1, len, aw->f) != len) {
        aw->ok = false;
        return ESP_FAIL;
    }

    /* RIFF 要求块长度是偶数：奇数就补一个 0（补的字节**不算**在本帧长度里） */
    uint32_t chunk_bytes = (uint32_t)len;
    if (len & 1u) {
        const uint8_t pad = 0;
        if (fwrite(&pad, 1, 1, aw->f) != 1) {
            aw->ok = false;
            return ESP_FAIL;
        }
        chunk_bytes += 1;
    }

    /* 累计 movi 负载：本帧块头(8) + 数据 + 填充。close 用它算 movi/riff 大小，
     * 不依赖 ftell（见文件头注释）。 */
    aw->movi_bytes += 8u + chunk_bytes;
    aw->frames++;
    return ESP_OK;
}

/* =====================================================================
 * close：回填三个字段 + fsync + 关文件
 * ===================================================================== */
esp_err_t avi_writer_close(avi_writer_t *aw, uint32_t *frames_out)
{
    if (aw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = (aw->ok && !ferror(aw->f)) ? ESP_OK : ESP_FAIL;

    /* movi LIST 大小 = 'movi'(4) + 负载 */
    const uint32_t movi_size = 4u + aw->movi_bytes;
    /* RIFF 大小 = 文件总大小 - 8 = 头长度 + movi 负载 - 8 */
    const uint32_t riff_size = (uint32_t)aw->hdr_end + aw->movi_bytes - 8u;
    patch_u32(aw->f, aw->pos_movi_size, movi_size);
    patch_u32(aw->f, aw->pos_riff_size, riff_size);
    patch_u32(aw->f, aw->pos_avih_frames, aw->frames);
    if (ferror(aw->f)) {
        ret = ESP_FAIL;
    }

    /* 一次 fsync 把数据 + FAT 表都刷下去。逐帧 fsync 会把 SD 写速拖垮，所以只在收尾刷一次。
     * 代价：录制中途断电会留下一个头没回填的文件（多半不可播）—— 本版接受。 */
    fsync(fileno(aw->f));
    fclose(aw->f);
    if (aw->wbuf != NULL) {
        heap_caps_free(aw->wbuf);
    }

    if (frames_out != NULL) {
        *frames_out = aw->frames;
    }
    free(aw);
    return ret;
}
