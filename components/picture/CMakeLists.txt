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
        "images/picture_icon_settings_data.c"
        "images/picture_icon_weather_data.c"
        # 天气条件图标（12 类 x 96px/32px）。由 原始图片/make_weather_icons.py
        # 生成 PNG，再用 LVGLImage.py 转成这里的 .c —— 别手改。
        "images/picture_icon_wmo_clear_data.c"
        "images/picture_icon_wmo_clear_s_data.c"
        "images/picture_icon_wmo_partly_cloudy_data.c"
        "images/picture_icon_wmo_partly_cloudy_s_data.c"
        "images/picture_icon_wmo_overcast_data.c"
        "images/picture_icon_wmo_overcast_s_data.c"
        "images/picture_icon_wmo_fog_data.c"
        "images/picture_icon_wmo_fog_s_data.c"
        "images/picture_icon_wmo_freezing_data.c"
        "images/picture_icon_wmo_freezing_s_data.c"
        "images/picture_icon_wmo_light_rain_data.c"
        "images/picture_icon_wmo_light_rain_s_data.c"
        "images/picture_icon_wmo_moderate_rain_data.c"
        "images/picture_icon_wmo_moderate_rain_s_data.c"
        "images/picture_icon_wmo_heavy_rain_data.c"
        "images/picture_icon_wmo_heavy_rain_s_data.c"
        "images/picture_icon_wmo_showers_data.c"
        "images/picture_icon_wmo_showers_s_data.c"
        "images/picture_icon_wmo_thunder_data.c"
        "images/picture_icon_wmo_thunder_s_data.c"
        "images/picture_icon_wmo_light_snow_data.c"
        "images/picture_icon_wmo_light_snow_s_data.c"
        "images/picture_icon_wmo_snow_showers_data.c"
        "images/picture_icon_wmo_snow_showers_s_data.c"
    INCLUDE_DIRS
        "include"
)

# LVGLImage.py 生成的 .c 靠一组宏决定怎么 include lvgl.h，默认会 fallback 到
# `#include "lvgl/lvgl.h"` —— 而本工程用的是 ESP-IDF 托管组件，那个路径并不存在。
# 走 ESP-IDF 时统一写 "lvgl.h" 就行，所以在这里定义这个宏。
# 定在组件级（而不是去手改生成的文件），重新生成时才不会把改动冲掉。
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)
