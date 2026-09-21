# ESP32-P4 多应用平台

在 **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B** 上自研一个"类手机"的多应用平台：
一个桌面 launcher + 若干可切换的 App，共用同一块 7 英寸触摸屏。

定位是**学习和验证**（框架、组件、外设 API 都自己搭一遍），不是产品 ——
所以每个 App 都更像"验证台"：只证明某条链路是通的，不做视觉打磨。

前身是 `../demo1`，USB 摄像头/JPEG 解码/SD 卡那几块是从它移植过来的。

## 硬件与工程配置

| 项 | 值 | 出处 |
|---|---|---|
| 板子 | Waveshare ESP32-P4-WIFI6-Touch-LCD-7B | `components/lcd_screen/` |
| 屏幕 | EK79007，MIPI-DSI，1024×600，RGB565，60Hz，双 framebuffer | `lcd_screen_display.c` |
| 触摸 | GT911，I2C（SDA 7 / SCL 8，地址 0x5D 或 0x14 双探测） | `lcd_screen_touch.c` |
| 背光 | LEDC PWM，GPIO32，低电平点亮 | `lcd_screen_display.c` |
| Flash | 32MB（app 8M + storage 23M，自定义分区表） | `partitions.csv` |
| PSRAM | 200MHz | `sdkconfig.defaults` |
| ESP-IDF | v5.4.3（200MHz PSRAM 需要 `CONFIG_IDF_EXPERIMENTAL_FEATURES`） | `sdkconfig.defaults` |
| LVGL | 9.5 + espressif/esp_lvgl_port ^2（组件管理器拉取） | `components/ui/idf_component.yml` |

## 编译与烧录

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p <PORT> flash monitor
```

首次构建会由组件管理器把依赖拉到 `managed_components/`（LVGL、esp_lvgl_port、
esp_lcd_ek79007、esp_lcd_touch_gt911、usb_host_uvc 等），不需要手动准备。

## 目录结构

```
main/main.c          唯一入口：只做"编排"（硬件 → 能力 → 注册 App → UI → 进主页）
apps/                内容层：每个 App 一个 .c，共用一个组件
                     （拍照保存原本是独立组件，现住在 apps/camera.c 里）
components/
  app_manager/       框架：App 注册表 + 启动/回主页，零 LVGL 依赖
  ui/                UI 层：把 LVGL 接到屏幕；ui_status_bar 是全局公共状态栏
  lcd_screen/        硬件层：面板与触摸初始化，只暴露句柄
  picture/           图片资源：壁纸与图标（数据由脚本生成）
  kv_store/          能力：键值存储（NVS 后端）
  time_service/      能力：时间（对时 + 时区，不认识任何时间源）
  sd_card/           能力：SD 卡挂载（之后用 POSIX 文件 API）
  frame_buf/         能力：双缓冲原语（丢旧保新 + 读槽占用保护）
  jpeg_decoder/      能力：JPEG → RGB565，上游可以是相机也可以是 SD 相册
  usb_camera/        能力：UVC 流 → 拼接成完整 JPEG 帧，并扇出一份给拍照
```

依赖方向单向：`main` → { `apps`, `ui` } → `lcd_screen`；能力组件之间不互相认识，
靠 `frame_buf` 这类"接口类型"解耦。

## 框架约定（加 App 前先看这段）

`app_manager` 只维护一张注册表，不知道 LVGL 是什么、也不知道有哪些 App：

- **第 0 个注册的 App 是主页**，`app_manager_go_home()` 切回它，所以桌面必须第一个注册。
- 切换顺序是 `enter(新) → leave(旧)`，新 screen 先就位，不会出现"没有有效 screen"的空窗。
- 各 App 的 `enter/leave` 里**自己负责加 LVGL 锁**（`lvgl_port_lock` 是递归锁，
  从任何上下文调用都不会死锁）。
- `leave` 里删自己的 screen 必须用 `lv_obj_delete_async()`，不能用 `lv_obj_delete()`
  —— 该回调可能正跑在本 screen 内某个对象的事件上。
- **`enter` 里申请的、游离于 screen 之外的资源，`leave` 里一个都不能漏**
  （LVGL 定时器挂在全局链表上，删 screen 不会带走它）。见 `apps/clock.c`。
- **谁常驻、谁动态**：进 App 才需要的重资源（相机 App 的解码器：约 1.4MB PSRAM +
  一条任务）在 `enter` 建、`leave` 拆；反过来，**有不可中断临界区**的东西必须常驻
  （拍照保存任务写盘期间持着 jbuf 读槽，中途被杀会让读槽永远释放不了）。
  完整说明见 `apps/camera.c` 顶部。
- 状态栏的标题/返回按钮由 `components/ui/ui_status_bar.h` 统一提供：
  App 在 `enter` 里 `ui_status_bar_apply()` 填标题，内容从 `UI_STATUS_BAR_HEIGHT`
  往下排，**自己不要画标题和返回按钮**。
- 全项目**不显式设置字体**，字号由 `CONFIG_LV_FONT_DEFAULT_MONTSERRAT_18` 一处决定。

## 现有 App

| App | 文件 | 做什么 / 验证什么 |
|---|---|---|
| Desktop | `apps/desktop.c` | 主页：遍历注册表生成图标栅格（6×3），点击启动；底部显示 free heap 便于观察进出 App 有无泄漏 |
| Demo | `apps/app_demo.c` | 验证 app_manager 调度 + kv_store 持久化：`count` 每次进入归零（说明页面真被重建），`total` 存在 NVS 里重启接着累加 |
| Clock | `apps/clock.c` | 每秒刷新的时间显示；演示"游离定时器必须在 leave 里删掉" |
| Camera | `apps/camera.c` | USB 摄像头预览（640×480 原样不缩放）+ 快门拍照存 SD 卡 |
| Photos | `apps/photo.c` | 相册：列 `/sdcard/DCIM` 里的 jpg，点开解码上屏；验证"SD 读文件 → 解码 → 上屏" |

## 加一个 App 的步骤

1. `apps/xxx.c` + `apps/include/xxx.h`（对外只暴露一个 `xxx_register()`），
   照抄 `apps/clock.c` 的形状：静态 `app_desc_t` + `enter/leave`。
2. `apps/CMakeLists.txt`：`SRCS` 加文件、`REQUIRES` 加依赖。
3. `main/main.c`：`#include "xxx.h"` + `xxx_register();`（桌面会自动多出一格）。
4. 图标（可选）：见下节。**没有图标也能跑** —— `picture_icon()` 查不到返回 NULL，
   桌面会只显示名字。

## 图像资源

壁纸和图标都是编译进固件的静态图片（`components/picture/images/*.c`），
由 `原始图片/` 下的源图经两个脚本生成：先 `make_icons_square.py`（裁到内容边界 +
四角抠透明，产出 96×96 带 alpha），再 `LVGLImage.py`（转成 RGB565A8 的 C 数组）。

完整流程、每个命令、以及"源图为什么不能带水印"都写在
**`components/picture/picture.c` 顶部的注释**里，加图标照着抄即可。
改完可以用 `原始图片/icons_square_preview.png` 直接看效果（脚本会把它贴在真实壁纸上）。

## 几个已经踩过的坑（都在代码注释里）

| 坑 | 去哪看 |
|---|---|
| frame_buf 必须"先还读槽再等新帧"，反了会死锁（症状：进去显示一帧就冻住） | `apps/camera.c` 的 `on_refresh` |
| PSRAM 对齐不能写死 64：L2 cache line 是 128B，不对齐解码器直接报错 | `components/frame_buf/frame_buf.c` |
| JPEG 输出缓冲尺寸/对齐由 IDF 校验，改分辨率要连着改三处 | `components/jpeg_decoder/include/jpeg_decode_task.h` |
| 拍照文件名必须扫目录取最大编号 +1，静态递增序号重启会覆盖旧照片 | `apps/camera.c` 的 `sd_next_photo_path` |
| 相册解码：上一次的读槽要"下次解码前"才还，早了会画到已释放内存 | `apps/photo.c` 的 `photo_show` |
| USB Host 两项配置是 UVC 能枚举的前提（不是调优项） | `sdkconfig.defaults` |
| SD 卡那条 LDO "voltage 0 out of range" 警告可忽略且无法消除 | `components/sd_card/sd_card.c` |
| 图标源图不能带水印，否则内容边界会变成整张图 | `components/picture/picture.c` 顶部 |

## 与 demo1 的关系

demo1 有的、这里已覆盖：桌面、app_manager、相机（+ 拍照存卡）、相册、屏幕/触摸、SD 卡。

这里多出来的：`kv_store`、`time_service`、全局状态栏、`picture` 图片资源组件、
Clock / Demo 两个 App，以及**每个 App 的 enter/leave 生命周期与资源回收纪律**
（demo1 的 App 是 `create_screen/destroy_screen`，没有回收约定）。

demo1 里有、这里**还没做**的：画板 App（`demo1/app/app_paint.c`）。
移植时唯一要小心的是画布缓冲区（PSRAM）的释放时机 —— 这里删 screen 是异步的，
buffer 不能紧跟其后就 free，得挂到对象的 `LV_EVENT_DELETE` 回调里。
