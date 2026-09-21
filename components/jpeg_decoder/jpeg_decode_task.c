#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/jpeg_decode.h"

#include "jpeg_decode_task.h"
#include "frame_buf.h"

#define TAG "jpeg_decode"

#define JPEG_DECODE_TASK_STACK 8192
#define JPEG_DECODE_TASK_PRIO  8
#define JPEG_DECODE_TASK_CORE  1
/* acquire 轮询超时：无新帧时定时醒来查 stop，保证销毁延迟 ≤100ms（原 1000ms 会拖慢销毁） */
#define JPEG_DECODE_STOP_POLL_MS     100
#define JPEG_DECODE_DESTROY_TIMEOUT_MS 500 /* destroy 等任务自删的兜底超时 */

struct jpeg_decode_s {
    frame_buf_t *in;           /* 输入 JPEG 帧缓冲：帧源侧（USB 相机 / SD 相册）创建并注入 */
    frame_buf_t *fb;           /* RGB565 输出缓冲：app 上屏 */
    TaskHandle_t task;
    volatile bool stop;        /* 销毁请求：任务循环检查，退出前释放解码引擎 */
    uint32_t decode_count;
    uint32_t last_report_tick;
    uint32_t last_out_size;
};

jpeg_decode_t *jpeg_decode_create(frame_buf_t *in)
{
    if (in == NULL) {
        return NULL;
    }
    jpeg_decode_t *jd = calloc(1, sizeof(*jd));
    if (jd == NULL) {
        return NULL;
    }
    jd->in = in;

    /* RGB565 输出缓冲：上屏消费，无需记录长度。
     * 本组件只有一个输出 —— 拍照要用的 JPEG 原数据由帧源侧（usb_camera）自己扇出，
     * 不从解码器这里取（见 usb_camera_get_jbuf 的说明）。 */
    const frame_buf_cfg_t fb_cfg = {
        .slot_size = JPEG_DEC_FRAME_BUF_SIZE,
        .has_len = false,
    };
    jd->fb = frame_buf_create(&fb_cfg);
    if (jd->fb == NULL) {
        free(jd);
        return NULL;
    }
    return jd;
}

void jpeg_decode_destroy(jpeg_decode_t *jd)
{
    if (jd == NULL) {
        return;
    }
    if (jd->task) {
        /* 置 stop 让任务在下一个 acquire 醒来时退出循环，
         * 任务自己释放解码引擎并自删；这里等它退出，避免硬删泄漏引擎 */
        jd->stop = true;
        uint32_t t0 = xTaskGetTickCount();
        while (jd->task != NULL &&
               (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(JPEG_DECODE_DESTROY_TIMEOUT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (jd->task != NULL) {
            /* 兜底：任务 100ms 内未自删（病态路径），强制删除（引擎将泄漏，仅防御） */
            vTaskDelete(jd->task);
            jd->task = NULL;
        }
    }
    /* in 由帧源侧创建，这里只释放解码器自建的输出缓冲 */
    frame_buf_delete(jd->fb);
    jd->fb = NULL;
    free(jd);
}

static void jpeg_decode_task(void *arg)
{
    jpeg_decode_t *jd = (jpeg_decode_t *)arg;

    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        /* 30ms：坏帧（源头截断 JPEG）快速释放，避免每帧死等 100ms 扣帧连累 UVC 侧；
           正常解码 4.9ms，裕量充足 */
        .timeout_ms = 30,
    };
    jpeg_decoder_handle_t decoder = NULL;
    esp_err_t err = jpeg_new_decoder_engine(&eng_cfg, &decoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_decoder_engine failed: %s", esp_err_to_name(err));
        jd->task = NULL; /* 让 destroy 的等待退出超时兜底结束 */
        vTaskDelete(NULL);
        return;
    }

    /* RGB565 输出。
       rgb_order=BGR 即小端输出，与 LVGL 字节序匹配（默认即此值，显式写出）。
       conv_std=BT601 为 MJPEG 摄像头常见标准。
       真机排查颜色：
       - 偏绿青(R 通道弱) -> 换 JPEG_YUV_RGB_CONV_STD_BT709 试
       - 偏蓝橙(R/B 颠倒) -> 换 JPEG_DEC_RGB_ELEMENT_ORDER_RGB 试 */
    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    jd->last_report_tick = xTaskGetTickCount();
    while (!jd->stop) {
        if (!frame_buf_wait_new(jd->in, JPEG_DECODE_STOP_POLL_MS)) {
            continue; /* 无新帧（设备未插/流未开），继续等待；销毁时 100ms 内醒来退出 */
        }

        uint32_t in_len = 0;
        const uint8_t *in_data = frame_buf_get_read(jd->in, &in_len);

        uint8_t *out = frame_buf_get_write(jd->fb);
        uint32_t out_size = 0;
        err = jpeg_decoder_process(decoder, &dec_cfg, in_data, in_len,
                                   out, JPEG_DEC_FRAME_BUF_SIZE, &out_size);
        if (err != ESP_OK) {
            /* 诊断坏帧内部结构，区分"驱动拼帧错乱"vs"摄像头源头坏帧"：
             * - SOI 计数>=2 或帧中间有 EOI 且后跟数据 -> 驱动把多帧拼一起（拼帧错乱）
             * - 仅 1 个 SOI 且无 EOI            -> 单帧截断/源头损坏 */
            uint16_t soi = (uint16_t)((in_data[0] << 8) | in_data[1]);
            uint16_t eoi = (uint16_t)((in_data[in_len - 2] << 8) | in_data[in_len - 1]);
            uint32_t soi_cnt = 0;
            int32_t last_eoi_off = -1;
            for (uint32_t i = 0; i + 1 < in_len; i++) {
                if (in_data[i] == 0xFF && in_data[i + 1] == 0xD8) {
                    soi_cnt++;
                }
                if (in_data[i] == 0xFF && in_data[i + 1] == 0xD9) {
                    last_eoi_off = (int32_t)i;
                }
            }
            ESP_LOGW(TAG, "process failed: %s len=%u soi=0x%04X eoi=0x%04X "
                     "soi_cnt=%u last_eoi_off=%d (frame end=%u)",
                     esp_err_to_name(err), (unsigned)in_len, soi, eoi,
                     (unsigned)soi_cnt, (int)last_eoi_off, (unsigned)(in_len - 2));
        } else {
            jd->last_out_size = out_size;
            frame_buf_commit(jd->fb, out_size);
            jd->decode_count++;
        }

        /* 坏帧诊断要扫 in_data，所以 read_done 必须在它之后：
         * 若提前 read_done，生产者可立即 commit 覆盖该槽，诊断会读到脏数据 */
        frame_buf_read_done(jd->in);

        uint32_t now = xTaskGetTickCount();
        if (now - jd->last_report_tick >= pdMS_TO_TICKS(1000)) {
            uint32_t elapsed = now - jd->last_report_tick;
            float fps = (float)jd->decode_count * 1000.0f / (float)elapsed;
            /* 这里是 demo1 原样的 ESP_LOGD。本项目 CONFIG_LOG_MAXIMUM_LEVEL = INFO，
             * 所以它在**编译期**就被剔除了 —— 既不打印、也不占空间。
             * 哪天想再看解码帧率，把 CONFIG_LOG_MAXIMUM_LEVEL 提到 DEBUG 即可
             * （运行期 esp_log_level_set 不行，那道闸在编译期）。 */
            ESP_LOGD(TAG, "Decoded %u frames (%.1f fps), out_size=%u",
                     (unsigned)jd->decode_count, (double)fps,
                     (unsigned)jd->last_out_size);
            jd->decode_count = 0;
            jd->last_report_tick = now;
        }
    }
    /* 销毁路径：释放解码引擎（任务内 malloc 的句柄，只有这里能释放），自删前标记退出 */
    jpeg_del_decoder_engine(decoder);
    jd->task = NULL;
    vTaskDelete(NULL);
}

esp_err_t jpeg_decode_start(jpeg_decode_t *jd)
{
    BaseType_t ret = xTaskCreatePinnedToCore(jpeg_decode_task, "jpeg_dec", JPEG_DECODE_TASK_STACK,
                                             jd, JPEG_DECODE_TASK_PRIO, &jd->task, JPEG_DECODE_TASK_CORE);
    if (ret != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

frame_buf_t *jpeg_decode_get_fb(jpeg_decode_t *jd)
{
    return jd->fb;
}
