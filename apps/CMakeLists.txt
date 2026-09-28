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
        "settings.c"
        "weather.c"
        "ime_test.c"
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
        avi_writer
        avi_reader
        frame_buf
        sd_card
        lcd_screen      # settings.c 直接用 LCD_SCREEN_*_RES 算布局
        wifi_service    # settings.c 扫描 / 设凭据 / 连接
        weather_service # weather.c 发起天气请求 / 轮询结果
        pinyin_keyboard # ime_test.c 的键盘 + 候选栏 + 输入法（自研：数字直选候选、中/英键、
                        #                简拼、用户学习，转换交给 pinyin_engine）
        pinyin_learn    # settings.c 的"清除输入习惯"（输入法的用户学习记录）
)
