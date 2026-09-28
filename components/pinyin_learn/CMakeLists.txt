# 输入法的「用户学习」持久化：把 pinyin_engine 里那张"词 -> 命中次数"的表存到 SD 卡。
#
# 依赖：
#   pinyin_engine  内存里的用户表（引擎不认识存储，这边负责喂入 / 取走）
#   sd_card        SD_CARD_MOUNT_POINT —— 文件路径常量挂在那上面
#
# 不需要 idf_component.yml：没有托管组件依赖。esp_log 是 IDF 对所有组件自动注入的。
idf_component_register(
    SRCS
        "pinyin_learn.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        pinyin_engine
        sd_card
)
