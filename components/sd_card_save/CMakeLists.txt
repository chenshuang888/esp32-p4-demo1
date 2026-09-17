# 拍照保存组件：把"按快门"和"写 SD 卡"解耦（独立任务 + 信号量 + 结果队列）。
#
# 文件操作走 POSIX（fatfs 注册的 VFS），所以 fatfs / vfs 都是**私有**依赖 ——
# 对外头文件里只出现 frame_buf_t 和 esp_err，不暴露任何文件系统类型。
# 和 sd_card 把 sdmmc/fatfs 放进 PRIV_REQUIRES 是同一个做法。
idf_component_register(
    SRCS
        "sd_card_save.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        frame_buf
    PRIV_REQUIRES
        sd_card
        fatfs
        vfs
        freertos
)
