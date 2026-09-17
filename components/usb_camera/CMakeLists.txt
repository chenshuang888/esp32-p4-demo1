# USB 摄像头（UVC）帧源。
#
# ⚠️ 它产出的不是"图像"，是**原始 MJPEG 帧** —— 要上屏必须再配一个
#    jpeg_decoder 消费 usb_camera_get_fb() 给出的帧缓冲。
#
# 帧通过 frame_buf 交给下游，所以 frame_buf 是**对外**依赖
# （usb_camera.h 里直接出现了 frame_buf_t）。
#
# usb_host_uvc 是远端托管组件，在 idf_component.yml 里声明、由组件管理器拉取；
# 托管组件的 CMake 名字不带命名空间前缀，所以这里照写 usb_host_uvc。
#
# 源文件来自 demo1 的同名组件，**原样照抄**（未做任何改动）。
idf_component_register(
    SRCS
        "usb_camera.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        usb
        usb_host_uvc
        esp_hw_support
        freertos
        frame_buf
)
