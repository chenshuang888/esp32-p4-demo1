# 刻意没有 REQUIRES：本组件是纯调度器，只用到 esp_err.h / esp_log.h（都是通用组件）
idf_component_register(
    SRCS
        "app_manager.c"
    INCLUDE_DIRS
        "include"
)
