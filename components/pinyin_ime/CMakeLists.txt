# 拼音输入法组件。
#
# dicts/ 下的 .c 是 gen_pinyin_dict.py 产出的纯数据，原样编进来即可，**永不手改**
# —— 要换字表/换排序就重跑脚本、覆盖它（见 scripts/gen_pinyin_dict.py）。
#
# 远端依赖（lvgl）在 idf_component.yml 里声明，由组件管理器自动加入。
# 托管组件的 CMake 名字带命名空间前缀（lvgl__lvgl），所以不要在 REQUIRES 里手写。
idf_component_register(
    SRCS
        "pinyin_ime.c"
        "dicts/lv_pinyin_dict.c"
    INCLUDE_DIRS
        "include"
)

# 生成的词典 .c 用 `#include "lvgl.h"` 才能拿到 lv_pinyin_dict_t 的定义。
# 走 ESP-IDF 托管组件时这个路径才对，所以在这里定义这个宏。
# 定在组件级（而不是去手改生成的文件），重新生成时才不会把改动冲掉。
# （components/font_cjk 与 components/picture 已踩过同一个坑，做法保持一致。）
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)
