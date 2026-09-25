# AVI 解封装器：从 MJPEG-in-AVI 文件里顺序取出 JPEG 帧。
#
# 它是**格式解码器**，不是相册业务 —— 和 avi_writer 对称：输入是一个文件，
# 输出是"一帧 JPEG 的裸指针 + 长度"。它不知道谁来播（相册 / 以后的别的 App），
# 也不认识 frame_buf、jpeg_decoder、LVGL。
#
# 对外头文件只用 <stdbool.h>/<stdint.h>/<stddef.h>/esp_err.h，内部用 newlib 的 FILE*。
# 唯一的外部依赖是 heap（要分配 128B 对齐的 DMA 缓冲），而 heap 是 IDF 对所有组件
# 自动注入的公共依赖，所以这里不需要手写 REQUIRES。
idf_component_register(
    SRCS
        "avi_reader.c"
    INCLUDE_DIRS
        "include"
)
