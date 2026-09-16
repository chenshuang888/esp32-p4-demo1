# 键值存储组件。
#
# 组件名不叫 nvs_store：NVS 只是**当前后端**，属于内部实现细节。以后若换成
# 文件或其他实现，只要这几个函数的行为不变，调用方一行都不用改。
#
# 对外头文件只用到 <stdint.h> / <stddef.h> / esp_err.h，不出现任何 NVS 类型，
# 所以 NVS 是私有依赖（PRIV_REQUIRES）——和 ui 对 lcd_screen 的做法一致。
idf_component_register(
    SRCS
        "kv_store.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        nvs_flash
)
