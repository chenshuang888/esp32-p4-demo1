# JPEG 解码器：消费上游 frame_buf 里的 JPEG，解成 RGB565 再发到自己的 frame_buf。
#
# 它**不知道帧从哪来** —— 上游可以是 USB 相机（MJPEG 流），也可以是 SD 相册
# （读文件）。这个解耦完全靠 frame_buf 这个接口类型实现（见 jpeg_decode_task.h
# 里 jpeg_decode_create() 的说明）。
#
# esp_driver_jpeg 是 IDF 内置组件，不需要从组件仓库下载、也不需要额外 sdkconfig。
#
# 源文件来自 demo1 的同名组件，**只有一处改动**：那句每秒统计的 ESP_LOGD 改成了
# ESP_LOGI（原因写在代码注释里 —— 本项目 CONFIG_LOG_MAXIMUM_LEVEL=INFO，
# ESP_LOGD 会在编译期被剔除）。
idf_component_register(
    SRCS
        "jpeg_decode_task.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_jpeg
        esp_hw_support
        esp_timer
        freertos
        frame_buf
)
