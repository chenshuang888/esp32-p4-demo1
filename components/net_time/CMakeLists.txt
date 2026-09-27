# 网络对时能力组件。
#
# REQUIRES lwip：esp_sntp.h 是 lwip 组件的公开头（lwip 的 include/apps 在它的
# 公开 INCLUDE_DIRS 里），不是独立组件 —— 所以这里直接依赖 lwip。
#
# ============================================================================
# ⚠️ 临时诊断当前**停用**（2026-09-27，定位完"SNTP 偶发 150 秒"之后）
#
# 文件还在，结论也写在里面：include/net_time_diag.h + net_time_diag.c。
# 它**不参与构建、不产生任何日志**，纯粹留着"以后万一又出问题"备查。
#
# 要重新启用，**下面三处必须一起放开**（少一处要么报错要么没探测）：
#   ① 本文件：SRCS 里的 "net_time_diag.c"
#   ② 本文件：REQUIRES 里的 esp_wifi（少了会报 "esp_wifi.h: No such file"）
#   ③ net_time.c：include "net_time_diag.h" + net_time_start() / on_sntp_sync()
#      里那三处（注释里都标了）
#
# ⚠️ 启用后每架次会**固定刷 300 秒**探测日志（约 200 行）—— 查完记得再关掉。
# ⚠️ 它不参与构建，所以平时不会被编译检查：以后 IDF/lwip 升级后重新启用时，
#    可能要顺手修几个 API。
# ============================================================================
idf_component_register(
    SRCS
        "net_time.c"
        # ⚠️ 临时诊断（下行链路探测），当前停用 —— 见上面的说明
        # "net_time_diag.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        lwip
        # ⚠️ 只为临时诊断加的（net_time_diag.c 要读 RSSI）—— 当前停用
        # 为什么是 esp_wifi 而不是 esp_wifi_remote：esp_wifi_remote 把
        # idf_v5.4/include/injected/ 这个目录 **prepend 到 esp_wifi 这个目标的
        # 接口头文件路径上**（esp_wifi_remote/CMakeLists.txt:56-60），也就是说
        # esp_wifi.h 挂在 esp_wifi 上；而 esp_wifi_remote 对 esp_wifi 只是
        # PRIV_REQUIRES，传不出来。少了这行就报 "esp_wifi.h: No such file"。
        # 同样的话 wifi_service 的 CMakeLists 里也写过一份。
        # esp_wifi
)
