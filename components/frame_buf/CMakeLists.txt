# 帧缓冲原语：双缓冲 + 非阻塞生产者 + 丢旧保新 + 读槽占用保护。
#
# 上游是帧源（USB 相机，以后的 SD 相册也是），下游是消费者（JPEG 解码器、
# 最终 LVGL 上屏）。它不认识任何一方 —— 只认"生产者提交帧 / 消费者取帧"
# 这一组操作，所以除了 FreeRTOS 和 heap 没有任何外部依赖。
#
# 源文件来自 demo1 的同名组件，**只有一处改动**：默认分配器不再写死 64 字节对齐，
# 改成 128（PSRAM 的 L2 cache 行上限）。demo1 的 cache line 是 64B、本项目是 128B，
# 写死 64 会让 JPEG 解码器直接报 "out_buffer size is not aligned"。
# 原因写在 frame_buf.c 里那段注释里。
idf_component_register(
    SRCS
        "frame_buf.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
        heap
)
