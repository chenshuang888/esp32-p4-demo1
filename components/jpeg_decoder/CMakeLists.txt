# JPEG 解码器：JPEG -> RGB565（**同步、一次性**）。
#
# 输入是裸指针 + 长度，输出是本组件自己持有的一块画布 —— 它**不知道帧从哪来**
# （USB 相机 / SD 相册都行），也**不认识 frame_buf**。
#
# 没有任务、没有锁：引擎和画布的生命周期跟着句柄走（App 进 enter 建、leave 拆）。
# 所以它只依赖 IDF 内置的 esp_driver_jpeg，不依赖任何自家组件。
#
# esp_driver_jpeg 是 IDF 内置组件，不需要从组件仓库下载、也不需要额外 sdkconfig。
idf_component_register(
    SRCS
        "jpeg_decoder.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_jpeg
        esp_hw_support
)
