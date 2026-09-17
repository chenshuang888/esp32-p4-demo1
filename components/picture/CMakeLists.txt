# 图片资源组件。
#
# images/ 下的 .c 是 LVGLImage.py 产出的纯数据，原样编进来即可，
# **永不手改** —— 要换图就重新生成、覆盖它。
#
# 远端依赖（lvgl）在 idf_component.yml 里声明，由组件管理器自动加入。
# 托管组件的 CMake 名字带命名空间前缀（lvgl__lvgl 等），所以不要在 REQUIRES 里手写。
idf_component_register(
    SRCS
        "picture.c"
        "images/picture_wallpaper_data.c"
        "images/picture_icon_clock_data.c"
        "images/picture_icon_demo_data.c"
        "images/picture_icon_camera_data.c"
        "images/picture_icon_photo_data.c"
    INCLUDE_DIRS
        "include"
)

# LVGLImage.py 生成的 .c 靠一组宏决定怎么 include lvgl.h，默认会 fallback 到
# `#include "lvgl/lvgl.h"` —— 而本工程用的是 ESP-IDF 托管组件，那个路径并不存在。
# 走 ESP-IDF 时统一写 "lvgl.h" 就行，所以在这里定义这个宏。
# 定在组件级（而不是去手改生成的文件），重新生成时才不会把改动冲掉。
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)
