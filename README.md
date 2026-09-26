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
  net_time/          能力：SNTP 对时（网络时间源；不认识 wifi，也不认识 time_service）
  wifi_service/      能力：WiFi（板载 C6 走 SDIO；**不认识 NVS**，凭据由调用方喂进来；
                     提供"拿到 IP 就通知"的回调，供 net_time 这类订阅者挂接）
  weather_service/   能力：天气（HTTPS 拉 Open-Meteo，**一次请求拿全"当前 + 24 小时 + 7 日"**；
                     自带常驻 worker 任务，**不认识 LVGL / wifi / time_service**
                     —— 时区交给请求里的 timezone=auto，由服务器换算成当地时间；
                     见坑表里"TLS 不能跑在 LVGL 任务里"）
  sd_card/           能力：SD 卡挂载（之后用 POSIX 文件 API）
  frame_buf/         能力：双缓冲原语（丢旧保新 + 读槽占用保护）
  jpeg_decoder/      能力：JPEG → RGB565（**同步一次性**；输入是裸指针，不认识 frame_buf）
  avi_writer/        能力：把一串 JPEG 帧写成 MJPEG-in-AVI 文件（同步；不认识相机/存储）
  avi_reader/        能力：从 MJPEG-in-AVI 文件里顺序取出 JPEG 帧（同步；不认识播放器）
  usb_camera/        能力：UVC 流 → 拼接成完整 JPEG 帧，并扇出两份给拍照与录像
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
- **谁常驻、谁动态**：进 App 才需要的重资源（相机的解码引擎 + 600KB 画布、相册的
  输入缓冲）在 `enter` 建、`leave` 拆；反过来，**有不可中断临界区**的东西必须常驻
  （拍照保存任务写盘期间持着 jbuf 读槽，中途被杀会让读槽永远释放不了）。
  完整说明见 `apps/camera.c` 顶部。
- 状态栏的标题/"回桌面"按钮由 `components/ui/ui_status_bar.h` 统一提供：
  App 在 `enter`（或每次切页）时 `ui_status_bar_apply()` 填标题，内容从
  `UI_STATUS_BAR_HEIGHT` 往下排，**自己不要再画一个回桌面的按钮**。
  ⚠️ App **内部**页面的"上一级"按钮不属于这一条 —— 那个语义是"退一层"，
  和状态栏的"回桌面"各管一段，App 可以自己画。见 `apps/settings.c`
  （它用 `< Settings` / `< WiFi` 这类措辞和状态栏的 `Back` 区分开）。
- 字体：全局默认是 **montserrat_18**（`CONFIG_LV_FONT_DEFAULT_MONTSERRAT_18`），
  不显式设字体就继承它 —— 大多数 App 都不需要设。
  **8~48 全档位已在 `sdkconfig.defaults` 里一次开满**（约 846 KB rodata，零运行期成本），
  所以需要不同字号时直接 `lv_obj_set_style_text_font(obj, &lv_font_montserrat_XX, 0)` 即可，
  不必再改配置。首个使用者是 Weather 的大温度（48 号）。
  ⚠️ 上限就是 48（LVGL 预置最大，没有 64/72/96）；>48px 的出路见 `sdkconfig.defaults` 的注释。

## 现有 App

| App | 文件 | 做什么 / 验证什么 |
|---|---|---|
| Desktop | `apps/desktop.c` | 主页：遍历注册表生成图标栅格（6×3），点击启动；底部显示 free heap 便于观察进出 App 有无泄漏 |
| Demo | `apps/app_demo.c` | 验证 app_manager 调度 + kv_store 持久化：`count` 每次进入归零（说明页面真被重建），`total` 存在 NVS 里重启接着累加 |
| Clock | `apps/clock.c` | 每秒刷新的时间显示；演示"游离定时器必须在 leave 里删掉" |
| Camera | `apps/camera.c` | USB 摄像头预览（640×480 原样不缩放）+ 快门拍照存 SD 卡 + 录像存 SD 卡（MJPEG 包成 AVI，PC 可直接播） |
| Photos | `apps/photo.c` | 相册：列 `/sdcard/DCIM` 里的 jpg + avi。照片点开解码上屏；视频点开自动播放（点画面暂停/继续，播完停在末帧再点回列表）。验证"SD 读文件 → （解封装）→ 解码 → 上屏" |
| Settings | `apps/settings.c` | 设置：设置列表 →（点 WiFi）WiFi 列表（扫描，按信号从强到弱排）→（点某个 SSID）输密码 → 连接。凭据存 NVS，开机由 main 读出来自动连。验证"扫描 → 选网 → 输密码 → 连上"整条链路 |
| Weather | `apps/weather.c` | 天气（手机风格）：左栏 hero（地点 + 48 号大温度 + 天气文字 + 大图标）+ 体感/湿度/风，右栏 24 小时横滑条 + 7 日预报；Refresh 按钮。验证"HTTPS 客户端 + 根证书包 + JSON 解析"整条链路；App 侧**一行网络代码都没有**，全在 weather_service 的常驻任务里。时区用请求里的 `timezone=auto` 让服务器换算，所以**不依赖 time_service**。天气图标为位图，WMO 码归成 12 类 × 96/32 两档（雨刻意分小雨/中雨/大雨），见 `components/picture/picture.h` |

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

**天气条件图标**走的是另一条路（源图形态完全不同）：
源图 `原始图片/天气.webp` 是"中国气象局标准天气图标"的**拼图** —— 白图标 + 紫青渐变底
+ 每个图标下方烤着中文标签，共 25 个。它不能用上面那套白底流程，所以另有一个
`原始图片/make_weather_icons.py`：按测好的坐标切片 → 用 **min 通道**抠 alpha
（背景 min ≤ 140、图标纯白，阈值 163 很干净）→ **重着色成深色**（浅色主题下白图标看不见）
→ 输出 96/32 两档 PNG → 再交给 `LVGLImage.py` 转成 C。
只取 12 类（另外 13 类 Open-Meteo 根本不会返回，理由写在该脚本顶部）。
效果预览：`原始图片/weather_icons_preview.png`。

## 几个已经踩过的坑（都在代码注释里）

| 坑 | 去哪看 |
|---|---|
| frame_buf 必须"先还读槽再等新帧"，反了会死锁（症状：进去显示一帧就冻住） | `apps/camera.c` 的 `on_refresh` |
| PSRAM 对齐不能写死 64：L2 cache line 是 128B，不对齐解码器直接报错 | `components/frame_buf/frame_buf.c` |
| JPEG 输出缓冲尺寸/对齐由 IDF 校验，改分辨率要连着改三处 | `components/jpeg_decoder/jpeg_decoder.c` |
| 拍照文件名必须扫目录取最大编号 +1，静态递增序号重启会覆盖旧照片 | `apps/camera.c` 的 `sd_next_media_path` |
| **往 SD 写大块時，源缓冲必须 128B 对齐**（`heap_caps_aligned_alloc(128, ...)`），否则 `sdmmc_write_sectors()` 会**静默**降级成 512B 单块写，速度掉 ~20 倍（实测 3.1MB/s → 0.13MB/s），且无任何报错 | `avi_writer.c` 顶部注释；`sdmmc_cmd.c:448` |
| 拍照与录像必须各用一份 frame_buf（`jbuf` / `rbuf`）：单读者只是约定，`frame_buf_get_read` 不看是否已有读者，共用会踩对方正在读的槽 | `usb_camera.c` 的 `frame_callback` |
| 录像任务必须常驻：写盘期间持着 rbuf 读槽，被杀会让读槽永远释放不了 | `apps/camera.c` 的 `record_task` |
| AVI 头的大小/帧数要等收尾回填；退出相机 App 前必须先停录并等它 `close`，否则留下的文件是坏的 | `apps/camera.c` 的 `camera_leave` / `avi_writer.c` |
| 相册解码：上一次的读槽要"下次解码前"才还，早了会画到已释放内存 | `apps/photo.c` 的 `photo_show` |
| USB Host 两项配置是 UVC 能枚举的前提（不是调优项） | `sdkconfig.defaults` |
| SD 卡那条 LDO "voltage 0 out of range" 警告可忽略且无法消除 | `components/sd_card/sd_card.c` |
| SNTP 必须在**拿到 IP 之后**才启动：lwip 自己的重试退避是 15s→逐次加倍→上限 150s，早启动会让首次对时白等很久。本项目把它挂在 `wifi_service` 的"拿到 IP"回调上 | `components/net_time/net_time.h`、`main/main.c` 的 WiFi 块 |
| **"换网"必须先 `esp_wifi_disconnect()`**：已经连着 AP 时 `esp_wifi_connect()` **不会切到新配置的 AP**（`esp_wifi.h:439` attention 2）—— 它返回 OK，但站还在旧 AP 上。症状极隐蔽：设了凭据、日志说"开始连接"，然后什么都没发生（实测：连着校园网去选自己的热点，一个断开事件都没有，请求照样跑在校园网上） | `components/wifi_service/wifi_service.c` 的 `wifi_service_connect()` |
| **TLS/HTTPS 不能跑在 LVGL 任务里**：那个任务的栈只有 **7168B**（esp_lvgl_port 的 `ESP_LVGL_PORT_INIT_CONFIG()` 默认值，`ui.c` 原样用的），放不下 TLS 握手 + 证书链校验。联网工作必须进自己的任务（IDF 官方 HTTPS 例程用的是 8192 栈的独立任务） | `components/weather_service/weather_service.c` 顶部 |
| 图标源图不能带水印，否则内容边界会变成整张图 | `components/picture/picture.c` 顶部 |
| 进 WiFi 列表时**扫描是同步阻塞的**（2~4 秒，界面冻住）；`lv_refr_now()` 是为了让 "Scanning..." 能显示出来，不然连提示都看不到 | `apps/settings.c` 的 `wifi_start_scan` |
| 中文 SSID 会显示成**一串占位方块**（项目没有中文字体）。不是空白 —— `LV_USE_FONT_PLACEHOLDER` 默认开会画方块 | `components/wifi_service/wifi_service.h` 的 `wifi_service_ap_t` |
| **横向滚动容器宽度必须是确定值**（`lv_pct(100)` 或固定 px）。用 `LV_SIZE_CONTENT` 时容器随子项长大，就没有可滚区，横滑**静默失效**；另外 flex 必须保持默认 `NOWRAP` | `apps/weather.c` 的 `s_hour_box` |
| `lv_obj_remove_style_all()` **会连 layout 一起清掉**，所以 `lv_obj_set_flex_flow/align` 必须在它**之后**调用（顺序反了：不崩，只是布局全错） | `apps/weather.c` 的 `body` / `apps/photo.c` |
| weather_service 的结果结构体约 304B，**不能放在 worker 的栈上**（那 8192B 的峰值被 TLS 握手占满）—— 用文件级 `static` | `components/weather_service/weather_service.c` 的 `s_report` |
| 请求不带 `forecast_hours=24` 时，hourly 会按 `forecast_days` 返回**满 7 天**（168 条），响应从 ~1.7KB 涨到 ~5.5KB 直接撑爆 body 缓冲 | `components/weather_service/weather_service.c` 的 `WEATHER_URL` |
| `LVGLImage.py` 在 Windows 上**吃不了中文路径**：argv 被按 ANSI 码页解码，`原始图片/...` 变乱码 → 报 `invalid input`（加 `python -X utf8` 也没用 —— 参数在进 Python 之前就已经坏了）。绕法：`cd` 进图标目录、只传 **ASCII 文件名** | `原始图片/make_weather_icons.py` 顶部的用法 |
| **`SPIRAM_XIP_FROM_PSRAM` 会让"多加图片/字体"表现为 free heap 掉几百 KB**：该配置把 flash 里的 `.rodata` + `.text` 整段在启动时搬进 PSRAM（`esp_psram.c` 的 `s_xip_psram_placement()`），且这段 PSRAM 是从可入堆的部分扣掉的；而桌面显示的 `esp_get_free_heap_size()` 把 PSRAM 算在内。所以 heap 少了**不是泄漏、也不是哪个 App 占的**，是固件体积的影子。本次加图标+字体后桌面 heap 掉了约 600KB | `sdkconfig.defaults` 的 PSRAM 段 |

## 与 demo1 的关系

demo1 有的、这里已覆盖：桌面、app_manager、相机（+ 拍照存卡）、相册、屏幕/触摸、SD 卡。

这里多出来的：`kv_store`、`time_service`、`net_time`（SNTP 对时）、`wifi_service`、`weather_service`（HTTPS + JSON）、全局状态栏、`picture` 图片资源组件、
Camera 的**录像能力**（`avi_writer` + 常驻录像任务）与相册的**视频播放**（`avi_reader`
+ 同步播放定时器）、Clock / Demo / Settings 三个 App，
以及**每个 App 的 enter/leave 生命周期与资源回收纪律**
（demo1 的 App 是 `create_screen/destroy_screen`，没有回收约定）。

demo1 里有、这里**还没做**的：画板 App（`demo1/app/app_paint.c`）。
移植时唯一要小心的是画布缓冲区（PSRAM）的释放时机 —— 这里删 screen 是异步的，
buffer 不能紧跟其后就 free，得挂到对象的 `LV_EVENT_DELETE` 回调里。
