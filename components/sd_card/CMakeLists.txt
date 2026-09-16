# SD 卡组件：只做挂载，文件操作交给 App 直接用 POSIX（fopen/opendir）。
#
# 对外头文件只用到 esp_err.h，不出现任何 sdmmc/fatfs 类型
# （所以砍掉了原来的 get_card() 之后，sdmmc 也顺势变成私有依赖）——
# 和 kv_store 把 nvs_flash 放进 PRIV_REQUIRES 是同一个做法。
idf_component_register(
    SRCS
        "sd_card.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        sdmmc
        esp_driver_sdmmc
        fatfs
)
