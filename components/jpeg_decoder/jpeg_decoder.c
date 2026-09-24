#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"

#include "jpeg_decoder.h"

#define TAG "jpeg_decode"

/* 一次解码最多等多久（引擎的时间上限）。
 * 正常解码实测 4.9ms，15ms 有约 3 倍裕量。
 *
 * ⚠️ 这个值现在的含义和以前不同了：它等于"**一次解码最多把调用方挡住多久**"。
 *    两个使用者（相机的刷新定时器、相册的点击回调）都跑在 LVGL 任务里，所以
 *    坏帧（源头截断的 JPEG）会让 UI 冻这么久。
 *    取小 = UI 少冻；但不能小于正常解码时间 —— IDF 的原话是
 *    "should larger than valid decode time in ms"（见 jpeg_decode.h 里 timeout_ms）。 */
#define JPEG_DECODE_TIMEOUT_MS 15

struct jpeg_decoder_s {
    jpeg_decoder_handle_t engine;   /* IDF 解码引擎（独占资源，同一时刻只能一个任务用） */
    uint8_t              *out;      /* 输出画布（本组件持有，地址/长度按 cache line 对齐） */
    size_t                out_size; /* 画布实际大小（分配器可能向上取整，原样传回引擎） */
};

jpeg_decoder_t *jpeg_decode_init(void)
{
    jpeg_decoder_t *jd = calloc(1, sizeof(*jd));
    if (jd == NULL) {
        ESP_LOGE(TAG, "calloc failed");
        return NULL;
    }

    /* 输出画布：**地址和长度都必须按 L2 cache line 对齐**（2DDMA 直接写 PSRAM，
     * 之后要按 cache 行做失效；不满足时引擎直接返回 ESP_ERR_INVALID_ARG）。
     *
     * 这里交给 IDF 的分配 helper 去做，而不是自己写死 128 —— 它内部用
     * esp_cache_get_alignment(MALLOC_CAP_SPIRAM, ...) 查实际值再对齐。
     * 见 esp_driver_jpeg/jpeg_decode.c 里 jpeg_alloc_decoder_mem() 顶部那段注释。
     * （顺带：同一个 helper 对**输入**缓冲不做任何对齐，所以调用方的输入槽
     *   用普通 PSRAM 分配就行 —— 相册那边就靠这条才不需要 frame_buf。） */
    const jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    jd->out = jpeg_alloc_decoder_mem(JPEG_DEC_FRAME_BUF_SIZE, &mem_cfg, &jd->out_size);
    if (jd->out == NULL) {
        ESP_LOGE(TAG, "output buffer alloc failed (%u bytes)", (unsigned)JPEG_DEC_FRAME_BUF_SIZE);
        free(jd);
        return NULL;
    }

    const jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,                       /* 0 = 让驱动自选默认中断优先级 */
        .timeout_ms    = JPEG_DECODE_TIMEOUT_MS,
    };
    esp_err_t err = jpeg_new_decoder_engine(&eng_cfg, &jd->engine);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_decoder_engine failed: %s", esp_err_to_name(err));
        heap_caps_free(jd->out);
        free(jd);
        return NULL;
    }

    return jd;
}

esp_err_t jpeg_decode_frame(jpeg_decoder_t *jd, const uint8_t *jpg, size_t len, uint8_t **out)
{
    if (jd == NULL || jpg == NULL || len == 0 || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* RGB565 输出。
       rgb_order=BGR 即小端输出，与 LVGL 字节序匹配（默认即此值，显式写出）。
       conv_std=BT601 为 MJPEG 摄像头常见标准。
       真机排查颜色：
       - 偏绿青(R 通道弱) -> 换 JPEG_YUV_RGB_CONV_STD_BT709 试
       - 偏蓝橙(R/B 颠倒) -> 换 JPEG_DEC_RGB_ELEMENT_ORDER_RGB 试 */
    const jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    uint32_t out_len = 0;
    esp_err_t err = jpeg_decoder_process(jd->engine, &dec_cfg, jpg, (uint32_t)len,
                                         jd->out, (uint32_t)jd->out_size, &out_len);
    if (err != ESP_OK) {
        /* 坏帧是预期内的（源头截断 / 拼帧溢出），所以只留一行：能看出"发生过、多大"，
         * 不再扫 SOI/EOI 去分辨原因（那个排查已结案）。
         * 注意 out_len 只在成功时有意义，这里不用。 */
        ESP_LOGW(TAG, "process failed: %s (len=%u)", esp_err_to_name(err), (unsigned)len);
        return err;                       /* out 不动：调用方继续显示上一帧 */
    }

    *out = jd->out;
    return ESP_OK;
}

void jpeg_decode_deinit(jpeg_decoder_t *jd)
{
    if (jd == NULL) {
        return;
    }
    if (jd->engine != NULL) {
        jpeg_del_decoder_engine(jd->engine);
        jd->engine = NULL;
    }
    if (jd->out != NULL) {
        heap_caps_free(jd->out);
        jd->out = NULL;
    }
    free(jd);
}
