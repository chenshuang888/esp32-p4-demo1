# 只用到 C 标准库(newlib)与 esp_log，都是通用组件，所以不需要 REQUIRES
idf_component_register(
    SRCS
        "time_service.c"
    INCLUDE_DIRS
        "include"
)
