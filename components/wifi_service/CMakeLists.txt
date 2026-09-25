# 托管组件（espressif__esp_wifi_remote / espressif__esp_hosted）由 idf_component.yml 声明。
# 组件管理器会把它们自动加进本组件的 REQUIRES，所以这里**不要**手写带命名空间的名字，
# 只列 IDF 自带的组件。
idf_component_register(
    SRCS
        "wifi_service.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_event       # esp_event_loop_create_default / esp_event_handler_instance_register
        esp_netif       # esp_netif_init
        lwip            # IPSTR / IP2STR 这两个打印 IP 的宏

        # ⚠️ 这一行必须写，而且**不是** esp_wifi_remote，虽然直觉上该是它。
        #    P4 没有射频，IDF 的 esp_wifi 组件会退化成"只提供 netif 绑定"的版本
        #    （esp-idf/components/esp_wifi/CMakeLists.txt:8-19）。
        #    esp_wifi_remote 干的事是：把 idf_v5.4/include/injected/ 这个目录
        #    **prepend 到 esp_wifi 这个目标的接口头文件路径上**
        #    （managed_components/espressif__esp_wifi_remote/CMakeLists.txt:55-60）。
        #    也就是说 esp_wifi.h 是挂在 esp_wifi 上的；而 esp_wifi_remote 对
        #    esp_wifi 只是 PRIV_REQUIRES，传不出来。
        #    少了这行就报 "esp_wifi.h: No such file or directory"。
        #
        #    esp_netif_create_default_wifi_sta() 也是这个组件提供的
        #    （它的 src/wifi_default.c），所以这行同时还解决了那个声明。
        #
        #    厂家示例没踩到这个坑，是因为它的 main/CMakeLists.txt 不写 REQUIRES ——
        #    IDF 对 main 有特殊待遇，会自动依赖构建里的所有组件。
        esp_wifi
)
