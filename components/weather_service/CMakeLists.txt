# 天气能力组件。
#
# 三个依赖都是 IDF 自带的（不是托管组件，所以要显式写）：
#   esp_http_client —— HTTPS GET（它公开依赖 lwip，所以 sys/socket.h 也拿得到）
#   json            —— cJSON：include "cJSON.h"
#   mbedtls         —— esp_crt_bundle.h 在 mbedtls 组件的公开 include
#                      （mbedtls/esp_crt_bundle/include/），用它的根证书包做校验
idf_component_register(
    SRCS
        "weather_service.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_http_client
        json
        mbedtls
)
