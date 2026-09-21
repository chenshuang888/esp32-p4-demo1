# 本组件在项目根目录、与 main 同级，靠根 CMakeLists.txt 的 EXTRA_COMPONENT_DIRS 被发现。
#
# 远端依赖（esp_lvgl_port / lvgl）在 idf_component.yml 里声明，由组件管理器自动加入；
# 托管组件的 CMake 名字带命名空间前缀（如 espressif__esp_lvgl_port），所以不要在
# REQUIRES 里手写它们，交给 manifest 更稳。
idf_component_register(
    SRCS
        "desktop.c"
        "app_demo.c"
        "clock.c"
        "camera.c"
        "photo.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        app_manager
        time_service
        kv_store
        ui
        picture
        usb_camera
        jpeg_decoder
        frame_buf
        sd_card
)
