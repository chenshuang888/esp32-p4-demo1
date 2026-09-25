# MJPEG-in-AVI 封装器：把一串独立的 JPEG 帧写成一个标准的 AVI 文件。
#
# 它是**格式编码器**，不是相机业务 —— 和 jpeg_decoder 同性质：输入是"一帧 JPEG
# 字节 + 长度"，输出是一个文件，它不知道帧从哪来（USB 相机 / 相册 / SD 文件都行），
# 也不认识 frame_buf、相机、LVGL。
#
# 对外头文件只用 <stdint.h>/<stddef.h>/esp_err.h，内部用 newlib 的 FILE*，
# 所以除了 IDF 的公共依赖（esp_common / newlib，IDF 对所有组件自动注入）不需要任何东西。
idf_component_register(
    SRCS
        "avi_writer.c"
    INCLUDE_DIRS
        "include"
)
