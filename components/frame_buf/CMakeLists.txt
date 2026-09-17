# 帧缓冲原语：双缓冲 + 非阻塞生产者 + 丢旧保新 + 读槽占用保护。
#
# 上游是帧源（USB 相机，以后的 SD 相册也是），下游是消费者（JPEG 解码器、
# 最终 LVGL 上屏）。它不认识任何一方 —— 只认"生产者提交帧 / 消费者取帧"
# 这一组操作，所以除了 FreeRTOS 和 heap 没有任何外部依赖。
#
# 源文件来自 demo1 的同名组件，**原样照抄**（未做任何改动）。
idf_component_register(
    SRCS
        "frame_buf.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
        heap
)
