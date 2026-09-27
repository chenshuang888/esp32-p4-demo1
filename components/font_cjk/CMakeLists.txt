# 中文字体组件。
#
# fonts/ 下的 .c 是 lv_font_conv 产出的纯数据，原样编进来即可，**永不手改**
# —— 要换字表或字号就重新生成、覆盖它（见 scripts/gen_font.sh）。
#
# 远端依赖（lvgl）在 idf_component.yml 里声明，由组件管理器自动加入。
# 托管组件的 CMake 名字带命名空间前缀（lvgl__lvgl），所以不要在 REQUIRES 里手写。
idf_component_register(
    SRCS
        "font_cjk.c"
        "fonts/lv_font_cjk_18.c"
    INCLUDE_DIRS
        "include"
)

# lv_font_conv 生成的 .c 靠一组宏决定怎么 include lvgl.h，默认会 fallback 到
# `#include "lvgl/lvgl.h"` —— 而本工程用的是 ESP-IDF 托管组件，那个路径并不存在。
# 走 ESP-IDF 时统一写 "lvgl.h" 就行，所以在这里定义这个宏。
# 定在组件级（而不是去手改生成的文件），重新生成时才不会把改动冲掉。
# （components/picture/CMakeLists.txt 已踩过同一个坑，做法保持一致。）
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)
