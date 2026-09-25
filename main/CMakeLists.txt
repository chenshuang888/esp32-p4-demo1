# 注意：main 只有"不写 REQUIRES"时才会自动依赖构建里的所有组件
#       （见 IDF 的 tools/cmake/project.cmake 里 "Special treatment for 'main'" 那段）。
#       一旦手写了 REQUIRES，这个自动行为就被关闭，用到什么必须显式列出来。
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES ui lcd_screen app_manager apps time_service kv_store sd_card wifi_service)
