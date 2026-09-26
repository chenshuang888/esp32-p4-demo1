# 网络对时能力组件。
#
# REQUIRES lwip：esp_sntp.h 是 lwip 组件的公开头（lwip 的 include/apps 在它的
# 公开 INCLUDE_DIRS 里），不是独立组件 —— 所以这里直接依赖 lwip。
idf_component_register(
    SRCS
        "net_time.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        lwip
)
