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
  font_cjk/          字体资源：中文（《通用规范汉字表》8105 字，18px）+ 把它接成全局默认；
                     英文/数字仍走默认字体，中文靠 LVGL 的 fallback 补上，可中英混排
  pinyin_engine/     输入能力：拼音 -> 汉字的**整句**转换引擎（纯 C，零依赖）
                     三种边：词边、字边、**缩写边（简拼）**；外加**词级用户学习**的内存表
                     + 词典数据（词表 30808 条 / 缩写表 5553 键 / 25281 音串，
                       约 1 MB rodata，由脚本生成）
  pinyin_keyboard/   屏幕上的那一套：**自研键盘**（`lv_buttonmatrix` 从零搭，**不用**
                     LVGL 的 lv_keyboard）+ 候选栏 + 输入法逻辑。5 行 × 3 页、数字行常驻、
                     左下角中/英切换、数字键直选候选。内部切两半：键盘那半只把按键翻成
                     "语义"报出去，**不认识输入框、也不认识引擎**
  pinyin_learn/      能力：用户学习记录的持久化 —— 存到 SD 卡、开机读回、App 退出时落盘、
                     可清除。**不认识 LVGL**；引擎反过来**不认识存储**，两边由 main 组装
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

- **中文**：`components/font_cjk/` 提供一套 **《通用规范汉字表》全表 8105 字（18px）**
  的位图字体，由 `main` 在 `ui_init()` 之后调 `font_cjk_install()` 接成全局默认。
  - 英文/数字仍由 Montserrat 渲染（观感不变），只有 Montserrat 没有的字形
    （中文）才回退到思源黑体 —— 靠的是 LVGL 的字体 `fallback`。
  - **依赖 `sdkconfig.defaults` 里的两个开关（都已开）**：
    - `CONFIG_LV_USE_FONT_COMPRESSED=y` —— 字体是 RLE 压缩格式（位图 967 KB；
      不压缩反而 1036 KB）。**不开的话中文一片空白，而且连占位方块都没有**
      （`LV_USE_FONT_PLACEHOLDER` 只管"字形找不到"，管不了"找到但取不到位图"）。
    - `CONFIG_LV_FONT_FMT_TXT_LARGE=y` —— 位图索引 20 位→32 位，解除 1 MB 上限
      （当前 967 KB = 94.5%，换字号/换更大字表立刻会超）。**一次开掉，以后不用再动配置。**
  - 字表取自 GitHub 的《通用规范汉字表》字表，并**与教育部系统的官方 PDF 全文逐字交叉验证**过
    （一级 3500 字顺序完全一致、去重恰为 8105 字、修正了 1 处误字）。其中 196 字在
    扩展B（补充平面）。重生成见 `components/font_cjk/scripts/gen_font.sh`；
    组装机制、踩过的两个坑、纵向度量的实测数据都写在 `components/font_cjk/font_cjk.c` 顶部。
  - ⚠️ 中文只做了 18px 一档，而 fallback 是"每个字体对象一份" —— 所以
    `apps/weather.c` 里显式设过字号的 label（48/24/20/16/22/14）**中文仍是方块**。
    要那些地方也显示中文，得为对应字号再生成一份。

## 现有 App

| App | 文件 | 做什么 / 验证什么 |
|---|---|---|
| Desktop | `apps/desktop.c` | 主页：遍历注册表生成图标栅格（6×3），点击启动；底部显示 free heap 便于观察进出 App 有无泄漏 |
| Demo | `apps/app_demo.c` | 验证 app_manager 调度 + kv_store 持久化：`count` 每次进入归零（说明页面真被重建），`total` 存在 NVS 里重启接着累加 |
| Clock | `apps/clock.c` | 每秒刷新的时间显示；演示"游离定时器必须在 leave 里删掉" |
| Camera | `apps/camera.c` | USB 摄像头预览（640×480 原样不缩放）+ 快门拍照存 SD 卡 + 录像存 SD 卡（MJPEG 包成 AVI，PC 可直接播） |
| Photos | `apps/photo.c` | 相册：列 `/sdcard/DCIM` 里的 jpg + avi。照片点开解码上屏；视频点开自动播放（点画面暂停/继续，播完停在末帧再点回列表）。验证"SD 读文件 → （解封装）→ 解码 → 上屏" |
| Settings | `apps/settings.c` | 设置：设置列表（WiFi / **Clear learned words**）→（点 WiFi）WiFi 列表（扫描，按信号从强到弱排）→（点某个 SSID）输密码 → 连接。凭据存 NVS，开机由 main 读出来自动连。验证"扫描 → 选网 → 输密码 → 连上"整条链路 |
| Weather | `apps/weather.c` | 天气（手机风格）：左栏 hero（地点 + 48 号大温度 + 天气文字 + 大图标）+ 体感/湿度/风，右栏 24 小时横滑条 + 7 日预报；Refresh 按钮。验证"HTTPS 客户端 + 根证书包 + JSON 解析"整条链路；App 侧**一行网络代码都没有**，全在 weather_service 的常驻任务里。时区用请求里的 `timezone=auto` 让服务器换算，所以**不依赖 time_service**。天气图标为位图，WMO 码归成 12 类 × 96/32 两档（雨刻意分小雨/中雨/大雨），见 `components/picture/picture.h` |
| IME Test | `apps/ime_test.c` | **验证台**（不是给用户用的功能）：自研键盘 + 输入框 + 整句拼音输入法（支持**简拼**），连打拼音 → 点候选 / 按空格 / **按数字 1-9 直选** → 回车提交到大字区。左下角键切**中/英**。选中的候选会被**学习**（下次提前）。验证"屏上能打出中文且渲染正确"这一条链路。零网络依赖，见"中文输入"一节 |

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

## 中文输入（拼音输入法）

**一套自研的**，从算法到键盘都是自己搭的，共三个组件：

| 组件 | 负责什么 |
|---|---|
| `pinyin_engine` | 算法：拼音串 -> 整句候选（纯 C、零依赖）。含简拼、词级用户学习 |
| `pinyin_keyboard` | 屏上那套：**自研键盘**（`lv_buttonmatrix` 从零搭）+ 候选栏 + 输入法逻辑 |
| `pinyin_learn` | 把用户学习记录存到 SD 卡（开机读回、App 退出时落盘） |

一次 `pinyin_keyboard_create()` 就把屏上那套全装好 —— 见下面的"用法"。

> 工程里曾经还有一版"薄接 LVGL 自带 `lv_ime_pinyin`"的实现（`components/pinyin_ime`），
> 已经删掉了：它的候选格每格只有 4 字节、**装不下词组**，而 `lv_keyboard` 的布局表又
> 改不了。要看那版：`git show 24e039d:components/pinyin_ime/`；踩过的坑留在坑表里。

### 引擎：拼音串 -> 整句候选

输入一串拼音 -> 引擎输出若干个**完整的**转换结果，点一个整段上屏：

```
jintiantianqi  ->  「今天天气」「今天天其」「今天天起」…
nihao          ->  「你好」「你号」「尼好」
bjdx           ->  「北京大学」…                ← 简拼（首字母缩写）
nhs + j        ->  「你好世界」                  ← 全拼/简拼可以混着打，引擎自己切
```

- **引擎**（`components/pinyin_engine/`，纯 C、零依赖）：把输入串的位置当节点，两两之间连边，
  **一共三种**：
  - **词边**：子串命中词表（`jintian` -> 今天）
  - **字边**：子串是合法音节（`hao` -> 好/号/毫…）
  - **缩写边（简拼）**：子串命中缩写表（`bjdx` -> 北京大学）

  每条边付"该词/字的量化代价 + 固定惩罚"，从 0 走到 n 的最小代价路径就是最优转换。
  每个节点保留 16 条最优路径（beam search），末端**去重**后给出 ≤12 个候选。

  **全拼 / 简拼 / 两者混着打，调用方一行都不用区分** —— 三种边在同一个图里，
  DP 自己找最优的切分方式。
- **词典**（`components/pinyin_engine/dicts/`，约 **1 MB** rodata）：
  jieba 的 35 万词表按语料频次筛 `≥100` -> **30808 词 / 25281 个音串**，
  用 pypinyin 做**词级**注音（自带多音字消歧）；缩写表是同一批词按"每字声母首字母"
  建的反查索引，**只存词表下标不重复存词串**，所以只有 ~106KB。
- **音串精确匹配天然绕开了分词**：打 `xianzai` 直接命中词表里的 `xianzai`，
  不用先判断是 xian+zai 还是 xi+an+zai。多音字同理：「银行」在 `yinhang` 和
  `yinxing` 两个 key 下各有一条记录，用户打哪个都能命中。
  （缩写表同理：长短语靠 DP 组合出来，`zhrmghg` = 中华+人民+共和国，不需要表里有它。）
- **实测准确率**：全拼 Top-1 28/30、Top-3 30/30；简拼 12/12（纯语料词频、未学习时）。
  用例表和自检实现在 `pinyin_engine/selftest.c`，**由 `main` 在启动时跑一遍并打进日志** ——
  之所以不做单元测试：这台机器上没有任何 host 编译器（gcc/clang/cc 全无）。
- 用法：
  ```c
  lv_obj_t *kb = pinyin_keyboard_create(body, ta, W, PINYIN_KEYBOARD_HEIGHT);
  lv_obj_add_event_cb(kb, on_submit, LV_EVENT_READY, NULL);   // 回车 = 提交
  ```
  一次调用把键盘、候选栏、输入法逻辑全装好。App 排版面时按 `PINYIN_KEYBOARD_TOTAL_H`
  （= 键盘 280 + 候选栏 44）给它们留高度，**不要自己 align 键盘** —— 键盘和候选栏是
  一体的，位置由组件摆（贴 `parent` 底部）。`leave` 里调 `pinyin_keyboard_detach()`
  （顺带把学习记录落盘）。
  提交事件仍然走 `LV_EVENT_READY`：键盘本身不认识"提交"，是本组件收到回车键后往键盘
  对象上补发一个 READY，所以 App 的写法一直没变。
  ⚠️ **`ta` 的内容归本组件管**：拼音字母是打在 `ta` 里的（拼音 + 已上屏汉字共用一个框），
  光标会被强制拉到末尾；拼音没上屏时不要从外面往里写字。`pinyin_keyboard_reset()`
  会把框里那串拼音一并删掉。
- 重新生成词典：`python components/pinyin_engine/scripts/gen_engine_dict.py`
  （幂等，含断言 + 写回自检 + 用同一套量化值跑一遍 Python 参考实现报准确率）

#### 键盘（`pinyin_keyboard`，自研）

**为什么自己做**：LVGL 的 `lv_keyboard` 布局表是 `lv_keyboard.c` 里的**文件内 static 数组** ——
外部既改不了键上写什么字，也**拿不到 ctrl_map**（只有 set、没有 get）。而"把中文输入
做顺手"要的恰好是这些。所以改用 `lv_buttonmatrix` 从零搭，只借用 LVGL 的通用控件层
（就像不去自己写渲染一样）。

布局（5 行 × 3 页：`abc` / `ABC` / `符号`）：

```
1 2 3 4 5 6 7 8 9 0        ← 数字行常驻，而且有候选时**数字 = 直选第 N 个**
q w e r t y u i o p            LVGL 自带的键盘把数字塞在 `1#` 页里，要翻页才能按
a s d f g h j k l
⇧ z x c v b n m ⌫
#+=   中/EN   ，  [  空格  ]  。  ↵
```

- **中/英在左下角**（真键盘也在这儿），键面直接写当前模式（`中` / `EN`）。
  切换时会顺带通知输入法那半，把没上屏的拼音"定案"
- 键表里每个键同时写清 **键面文字 / 语义 / 相对宽度**，而**分发只看语义、不看文字** ——
  所以 `中/EN` 那个键的文字随模式变，也不会让按键识别错位
- 宽度是**同一行内的相对值**（1..15），LVGL 按比例分完整行；所以"一个键多宽"取决于
  它和同行其它键的比值，而不是绝对值（第三行 9 个键自然比上面两行的 10 键宽一点）

**组件内部切了两半**（`pinyin_keyboard.c` 上半是键盘、下半是输入法）：

- **键盘那半不认识输入框、也不认识引擎**：只把按键翻成"字母/数字/标点/退格/空格/回车"
  报出去。正因如此，"数字键到底是直选候选还是插字符"由**输入法那半**拍板 ——
  只有它知道当前有没有候选
- 这条边界**跟"两半在不在一个文件里"无关**，它是防止键盘逻辑变浑的东西（键盘不该
  去猜"现在有没有候选"）。接缝就是那一个 `on_key()`

⚠️ **换页/换模式之后必须重贴 ctrl_map**：`lv_buttonmatrix_set_map()` 会重建按键区域、
把宽度和键的灰色底一起冲掉。所以这两件事永远成对做，走 `apply_page()`。
（曾经在"切中/英"那条路上漏过一次 —— 症状是**一切语言，所有键宽退回默认、灰键变白**。）

⚠️ **布局表里的 `"\n"` 不是按键**：它只表示换行，而
`lv_buttonmatrix_get_selected_button()` 返回的下标**不含**它。所以键盘内部维护两套下标
（布局表下标 / 按键下标），在建表时一起生成 —— 见 `build_page()`。

⚠️ **外观是自己显式设的，不靠主题**：主题按 `lv_buttonmatrix_class` 给的是"卡片"外观
（圆角 + 边框），而键盘要是"铺满一条"；另外主题只给 `lv_keyboard_class` 加"键背景白 +
键专用底色"这两条，纯 buttonmatrix 拿不到。所以 `apply_style()` 里全部显式设一遍
（`lv_obj_set_style_*` 是局部样式，优先级高于主题样式）。

#### 简拼（首字母缩写）

缩写键 = 每个字的**声母首字母**（zh/ch/sh 取 z/c/s；零声母取音节首字母），
所以「你好世界」-> `nhsj`、「北京大学」-> `bjdx`。**从词的汉字用 pypinyin 算**，
不能从音串推 —— 音串里没有音节边界；而且必须用**词级**注音，否则「银行」会按单字
「行」的主读音得到 `yx`（正确是 `yh`）。

键打包成 **uint32**（4 槽 × 5bit，a-z -> 1..26），这样整数大小关系与字符串字典序一致，
能直接对整数二分 —— 省掉 `strcmp`，也省掉"字符串池 + 偏移数组"那一整套。

每组还按**组内词数**罚一个 `penalty = round(log(组内词数) × COST_SCALE)`：
缩写比全拼歧义得多（实测 5553 个键里，中位数组只有 1 个词，最大的 `zz` 有 248 个），
不罚的话打 `da` 会先出「答案」而不是「大」。

⚠️ **简拼的天花板是语料词频，不是算法**。实测（缩写组内按词频排名）：

| 词 | 简拼 | 组内词数 | 排名 |
|---|---|---|---|
| 中国 | `zg` | 88 | 1 |
| 北京 | `bj` | 108 | 1 |
| 世界 | `sj` | 165 | 1（和「时间」代价打平） |
| **你好** | `nh` | 32 | **9** |
| **手机** | `sj` | 165 | **11** |
| **谢谢** | `xx` | 110 | **26** |

后三个**简拼打不出来** —— 因为 jieba 是新闻语料，「南海」比「你好」常见。
这不是 bug，是"语料词频 ≠ 用户的使用习惯"。**解法就是下面的用户学习。**

#### 用户学习（词级）

用户每选中一个候选，引擎从该候选的路径**回溯出分词**，把路径上的每个词计数 +1；
下次转换时这些词的代价被压低（`bonus = min(count, 8) × 2`，上限 16 单位）。

⚠️ **学习必须是"词级"的，不能是"输入串 -> 文本"那种整串记忆。** 上表最后一列说明
了原因：用户打 `nh` 想要「你好」，而它在候选栏里**根本不存在**（第 9 名，取不到）——
**他选不到它，也就永远教不会系统**。只有"用户先用全拼 `nihao` 打几次、选中「你好」→
「你好」这个词的代价下降 → 它在 `nh` 组里自动爬上来"才能解开这个死锁。
`selftest.c` 里有一段专门验收这条闭环（学之前不在候选、学之后进 Top-3）。

⚠️ 光"减代价"**还不够**：词边/缩写边原来是"取组里前 N 条"，缩写组最大 248 个词而
只取前 16 —— 第 17 名以后的词把代价压到 0 也进不了候选池。所以边收集改成了
"扫完整组、按调整后代价挑前 N"，并且 **`count > 0` 的词无条件建边**（组内硬上限 32）。

存储：`/sdcard/pinyin_learn.tsv`（一行一条 `词<TAB>次数`，可读可手改）。
`components/pinyin_learn/` 负责读入/落盘/清除；**引擎不认识存储**，两边由 `main` 组装
（同 wifi_service 收凭据）。**落盘只在 App 退出（detach）时做一次** —— 每选一个词就写卡
会有写放大，而写卡是同步阻塞的，调用方跑在 LVGL 任务里。没插卡就降级为纯 RAM
（本次开机内学习照常生效，只是不持久化）。
设置 App 里有一条 **"Clear learned words"** —— 学习表会脏，没有出口就下不来。

### 曾经的那一版：`pinyin_ime`（已删除）

工程里原来还有一版输入法：**薄接入 LVGL 9.5 自带的 `lv_ime_pinyin`**
（`managed_components/lvgl__lvgl/src/widgets/ime/lv_ime_pinyin.c`），我们只提供词典 +
绕开它两个坑，逐字选候选。**已经删掉了**，原因：

- 它的候选格每格只有 4 字节（源码里的 `lv_pinyin_cand_str[][4]`），**装不下词组** ——
  所以 `nihao` 要分两次选字；
- 它要的键盘是 LVGL 自带的 `lv_keyboard`，而那个的布局表我们改不了（见上面"键盘"一节）；
- 开着它的那几项 Kconfig 还带来"改配置必须先删 `sdkconfig`"的副作用。

要看那版实现：`git show 24e039d:components/pinyin_ime/`。
**它踩过的坑留在下面的坑表里** —— 那几条讲的是 LVGL 内部行为（候选串必须 3 字节对齐、
候选栏字体不继承、键盘左下角键会把 IME 弄坏），不只是旧组件的事。

⚠️ **不要给 WiFi 密码那种字段挂输入法** —— `apps/settings.c` 的密码框用的是 LVGL 自带的
`lv_keyboard`，本来就没挂 IME。那种字段要的是原样 ASCII，挂上输入法反而会把输入的字母
当拼音吃掉。

### 已知限制

- **没有用户词频自学习** → **已实现**（见上面"用户学习"）。但它**只能学词表里已有的词**：
  用户选不到的词（例如「多少钱」，词表里根本没这条）学不了 —— 真正的"自造词"需要一个
  专门的录入界面，本次没做。
- **简拼首屏可能没有目标词**：像「谢谢」在 `xx` 组里排第 26，即使取前 16 也够不到。
  出路是靠用户学习逐步把它顶上来，或者回退全拼。**没做候选栏翻页**（超出本次范围）。
- **简拼只覆盖 2~4 字词**：更长的短语靠 DP 组合（`zhrmghg` → 中华+人民+共和国），
  不是从表里直接命中的 —— 所以"组合出来的"那个结果可能输给某个恰好整个匹配的罕见词
  （实测 `nhsj` 不是「你好世界」，因为「你好世界」不在词表里）。
- **不支持"部分重选"**：整句当一个候选，第一个不对就换一个候选或退格重来；
  不能在已上屏的句子里单独改某个词（真实输入法有，要拆词重排）。
- **拼不出就不给候选**：`nihap` 直接返回 0（不会退而给出"部分转换"，
  那串拼音还留在输入框里，可以退格）。
- **数字直选只覆盖 `1`~`9`**：候选最多 12 个，第 10 个往后只能点候选栏。
- **拼音打到一半按标点，会先把拼音"定案"**：打 `ni` 再按 `，` 得到的是字面量 `ni，`
  而不是「你」+ 逗号 —— 因为拼音写在输入框里，中途插字符会让"末尾有几个字符是拼音"
  这笔账错位，所以规则是"先把拼音定案、再插入"。**中文里夹英文数字靠的就是这个代价**。
  （数字键是**例外**：有候选时它是直选，见上。）
- **键盘没有长按弹层、滑动输入、九宫格**：这个验证台不需要，都没做。
- **中文只有 18px 一档** —— `font_cjk` 的既有约束，所以输入框/候选栏都不能显式设字号。
- **词表是新闻语料频次**：口语词偏少，且漏了 `多少钱`（它在 jieba 里频次只有 3）。
  放宽阈值可改善，但数据体积线性涨（参数说明在生成脚本顶部）。
- **学习记录在 App 退出时才落盘**，会阻塞 LVGL 任务几十毫秒（那会儿正在切页，看不出来）；
  代价是"选完词立刻拔电"会丢最后一批。若实测卡顿，再考虑挪到独立任务。

（已删除的 `pinyin_ime` 另有"只能单字候选""196 个 4 字节字打不出来"等限制 ——
那些随组件一起没了，要看的话 `git show 24e039d:components/pinyin_ime/`。）

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
| **压缩字体 + `LV_USE_FONT_COMPRESSED` 没开 = 中文一片空白，而且连占位方块都没有**。`lv_font_conv` 默认输出 RLE 压缩位图（生成物里 `.bitmap_format = 1`），而 LVGL 的压缩解码器默认是关的；此时 `lv_font_get_bitmap_fmt_txt()` 会直接 `return NULL`（`lv_font_fmt_txt.c` 的 `#else /*!LV_USE_FONT_COMPRESSED*/` 分支）。因为字形是"**找到了**、只是取不到位图"，`LV_USE_FONT_PLACEHOLDER` 也轮不上画方块 —— 极易误判成"字体没生效"。接中文字体时踩过 | `components/font_cjk/font_cjk.c` 顶部；`sdkconfig.defaults` 的中文字体段 |
| 中文 SSID 曾经显示成**一串占位方块**（那时项目还没有中文字体）。现已由 `components/font_cjk` 覆盖；但 SSID 是**裸字节**，本身得是 UTF-8 才能渲染（发 GBK 的路由器仍是方块），且 `%.32s` 按字节截断可能切出半个汉字 | `components/font_cjk/font_cjk.c`、`apps/settings.c` 的行格式化处 |
| **横向滚动容器宽度必须是确定值**（`lv_pct(100)` 或固定 px）。用 `LV_SIZE_CONTENT` 时容器随子项长大，就没有可滚区，横滑**静默失效**；另外 flex 必须保持默认 `NOWRAP` | `apps/weather.c` 的 `s_hour_box` |
| `lv_obj_remove_style_all()` **会连 layout 一起清掉**，所以 `lv_obj_set_flex_flow/align` 必须在它**之后**调用（顺序反了：不崩，只是布局全错） | `apps/weather.c` 的 `body` / `apps/photo.c` |
| weather_service 的结果结构体约 304B，**不能放在 worker 的栈上**（那 8192B 的峰值被 TLS 握手占满）—— 用文件级 `static` | `components/weather_service/weather_service.c` 的 `s_report` |
| 请求不带 `forecast_hours=24` 时，hourly 会按 `forecast_days` 返回**满 7 天**（168 条），响应从 ~1.7KB 涨到 ~5.5KB 直接撑爆 body 缓冲 | `components/weather_service/weather_service.c` 的 `WEATHER_URL` |
| `LVGLImage.py` 在 Windows 上**吃不了中文路径**：argv 被按 ANSI 码页解码，`原始图片/...` 变乱码 → 报 `invalid input`（加 `python -X utf8` 也没用 —— 参数在进 Python 之前就已经坏了）。绕法：`cd` 进图标目录、只传 **ASCII 文件名** | `原始图片/make_weather_icons.py` 顶部的用法 |
| **`SPIRAM_XIP_FROM_PSRAM` 会让"多加图片/字体"表现为 free heap 掉几百 KB**：该配置把 flash 里的 `.rodata` + `.text` 整段在启动时搬进 PSRAM（`esp_psram.c` 的 `s_xip_psram_placement()`），且这段 PSRAM 是从可入堆的部分扣掉的；而桌面显示的 `esp_get_free_heap_size()` 把 PSRAM 算在内。所以 heap 少了**不是泄漏、也不是哪个 App 占的**，是固件体积的影子。本次加图标+字体后桌面 heap 掉了约 600KB | `sdkconfig.defaults` 的 PSRAM 段 |
| 中英混排（字体 fallback）要核对**纵向度量**：回退字形的纵向位置用**它自己**的 `ofs_y`，但行高取**主字体**的 `line_height`。两者差得多就会把字裁掉。本项目实测 Montserrat 18 行盒 17/4，思源黑体汉字 16/2、常用中文标点 ≤16/3 —— 装得下，所以不用改 `line_height` | `components/font_cjk/font_cjk.c` 顶部 |
| **`lv_obj_add_style()` 加的样式，优先级低于主题的样式 —— 所以它盖不住主题设的字体**。对象内部样式数组是 `[transition][local][normal]`，取值时**正序遍历、同 state 命中即返回**（数组靠前 = 优先级高）。`lv_obj_add_style` 加的是 normal，追加在主题样式**后面** → 被无视。`lv_obj_set_style_*()` 走 `lv_obj_set_local_style_prop()` → local → 排在 normal 前面 → 才能覆盖。**症状就是我们第一次接入中文字体时"设了却显示不出来"** | `components/font_cjk/font_cjk.c` 顶部"⚠️"一节；`lv_obj_style.c:123-131`、`:830-868` |
| 自定义字体当"全局默认"**不能走 Kconfig**：`CONFIG_LV_FONT_DEFAULT_*` 那个 choice 里只有 LVGL 内置字体。而主题只在**根对象**上设字体（`lv_theme_default.c` 的 `theme_apply()` 里 `parent == NULL` 那条分支），子对象靠继承 —— 所以要么改配置，要么自己挂一条主题链 | `components/font_cjk/font_cjk.c` 的 `root_font_apply` |
| `lv_layer_top()` / `lv_layer_bottom()` 在 `lv_init()` 阶段就建好了，**主题是在那之前应用**的；而 `lv_display_set_theme()` 基本不重刷已有对象。所以运行期换主题后，已存在的根对象吃不到新主题，得显式补一遍（状态栏就挂在 layer_top 上） | `components/font_cjk/font_cjk.c` 的 `font_cjk_install` |
| 从 `main` 任务调 LVGL 必须 `lvgl_port_lock()`：LVGL 有自己的任务在跑，不持锁就是竞态 | `components/ui/ui_status_bar.c` 的 `ui_status_bar_init`、`components/font_cjk/font_cjk.c` |
| **LVGL 自带拼音 IME 的候选串必须"每字 3 字节"**：`cand_num = strlen(py_mb)/3` 且按 3 字节切片（`lv_ime_pinyin.c` 的 `pinyin_input_proc`）—— 掺进 4 字节字（CJK 扩展 B/C/D/E）后，该音节**从那里起所有候选全部错位成乱码**。所以生成词典时整字剔除 196 个 4 字节字（该组件已删，看快照） | `git show 24e039d:components/pinyin_ime/scripts/gen_pinyin_dict.py` |
| **候选栏的中文不会自己继承字体**：候选栏是 `lv_buttonmatrix`，它的父对象是**键盘的父对象**（是 IME 对象的**兄弟**，不是子对象），所以不从 IME 继承。IME 只在收到 `LV_EVENT_STYLE_CHANGED` 时把字体转过去。必须显式给候选栏设一次字体，否则满屏方块 | `git show 24e039d:components/pinyin_ime/pinyin_ime.c` 的 `apply_cand_font` |
| **键盘左下角那个键（`LV_SYMBOL_KEYBOARD`）会把输入法弄坏**：IME 把它当"切 9 键模式"，而 `lv_ime_pinyin_set_mode()` 里 `mode = mode;` 是**无条件**执行的、换键盘布局那段却被 `#if LV_IME_PINYIN_USE_K9_MODE` 包着 —— 本项目不开 K9，于是按一下变成"模式是 K9、布局还是 K26"，之后**所有字母都不再出候选**（不崩、不报错）。绕法：追加一个 `VALUE_CHANGED` 回调把模式按回去 | `git show 24e039d:components/pinyin_ime/pinyin_ime.c` 的 `on_kb_value_changed` |
| 拼音词典**必须按 py 升序且同首字母连续**：`init_pinyin_dict()` 靠"首字母变化"建 `py_pos[26]`/`py_num[26]`，检索是在该字母区间里线性扫。排错 = 索引错乱、查不到字（脚本里有断言守住） | `git show 24e039d:components/pinyin_ime/scripts/gen_pinyin_dict.py` |
| 大写模式下输入法**不工作**（字母直接以 ASCII 进 textarea）—— 这是**需要的特性**（输 API key / URL 就是要这样），不是缺陷。IME 只接小写 `a-z` | `git show 24e039d:components/pinyin_ime/include/pinyin_ime.h` 的"已知限制" |
| 候选栏位置**不能用 flex 容器摆**：它的位置是 `lv_obj_align_to(cand, kb, ...)` 算的，而 flex 会接管子对象位置、把 align 覆盖掉（不崩，只是布局全错）。所以 `apps/ime_test.c` 整页刻意用显式 align | `apps/ime_test.c` 顶部版面说明 |
| **整句转换的 DP 里，边的跨度上限要按"最长音串"而不是"最长音节"**：最长音节 6 字母（zhuang），但一个双字词的音串能到 12 字母（beijing=7、tushuguan=9）、最长 20。按 6 写的话**所有双字以上的词全都不会被考虑**，Top-1 直接 28/30 → 14/30。字边才用"最长音节"限制 | `components/pinyin_engine/pinyin_engine.c` 顶部"两个容易写错的地方" |
| DP 末尾**必须去重**：不同路径经常产出同一个文本（词边「你好」和字边「你+好」都得 "你好"），不去重候选栏里会出现两个一模一样的候选。去重会消耗候选数，所以每节点保留的路径数要比候选数多几个 | `pinyin_engine.c` 的 `PE_PATH_KEEP`；`gen_engine_dict.py` 的同名常量 |
| **转换引擎的路径表不能放栈上**（约 14KB）：调用方是 LVGL 任务，它的栈只有 7168B。所以放文件级 static，代价是函数不可重入 | `components/pinyin_engine/pinyin_engine.c` 顶部 |
| **不能在候选按钮的点击回调里删或重建候选对象**：点候选会上屏并刷新候选栏，而此刻那个按钮的事件正在跑，删它（或它父对象的孩子）会崩。所以候选按钮一次建好，之后只改文本/显隐 | `components/pinyin_keyboard/pinyin_keyboard.c` 的 `refresh_bar` |
| **输入法控件把键盘/输入框指针存在 static 里**：App 的 `leave` 必须调 `pinyin_keyboard_detach()`。忘了不会崩（再 create 会覆盖），但显式 detach 才符合"游离资源 leave 里一个都不能漏"那条纪律 | `components/pinyin_keyboard/pinyin_keyboard.c` |
| **`lv_obj_get_width()` 读的是"缓存坐标"，不是"你设进去的值"**：刚创建（或刚改过尺寸）、还没经过一次布局的对象，`coords` 全是 0。在 `enter()` 里拿它去定另一个对象的尺寸会得到 **0**，而容器默认裁剪子对象 → **整块东西一片空白**。`lv_obj_pos.h` 的 `@note` 写得很明白："坐标只在下一次重绘时才重算"，绕法是取值前先 `lv_obj_update_layout(obj)`。实测症状极具迷惑性：**敲拼音什么都不显示，但 ASCII 直插照常能用**（那条路不碰出问题的那块）。注意 `lv_obj_align_to()` 内部**会**先刷布局（所以它自己能对齐对），是"在调它之前读坐标"才出事 | `components/pinyin_keyboard/pinyin_keyboard.c` 的 `build_cand_bar` |
| **二分查到"某一条"不等于查到"第一条"**：词表里同一个音串有**多条**记录（多个同音词，按词频降序连续存放）。二分只保证命中其中之一，落在中间就会让"取前 N 条边"从中间开始数，**把最高频的几个词整个跳过去** —— 症状是整句候选里最常见的那个词反而不出现（实测 `beijing` 出「背景」不出「北京」、`keyi` 出「可疑」不出「可以」）。所以命中后必须退到组头。这个 bug **Python 参考实现抓不到**（它用 dict，天然从组头开始），是设备上的运行期自检抓出来的 | `components/pinyin_engine/pinyin_engine.c` 的 `pinyin_engine_convert` |
| **pypinyin 取一个字的全部读音要用 `pinyin(c, heteronym=True)[0]`**，写成 `[x[0] for x in pinyin(...)]` 只会拿到**第一个**读音 —— 多音字的次要读音全丢，结果是"谁"打不出 `shei`、"这"打不出 `zhei`、"得"打不出 `dei`（实测少 13 个音节）。返回结构是 `[[读音1, 读音2, ...]]`，一整层才是"这个字的全部读音" | `components/pinyin_engine/scripts/gen_engine_dict.py` 的 `build_syllable_table` |
| **生成物要 include 对头文件**：维度常量（`PE_WORD_COUNT` 等）在 `pe_dict.h` 里，生成物只引 `pinyin_engine.h` 会编不过（`'PE_WORD_COUNT' undeclared`）。生成脚本的自检已补上这条 —— 第一版自检只数了记录条数，全是数据层面的检查，漏了 include | `components/pinyin_engine/scripts/gen_engine_dict.py` 的 `check_emitted` |
| **"减代价"救不了被截断的词**：词边/缩写边原本是"取组里前 N 条"，而缩写组最大 248 个词、只取前 16 —— 排第 17 名以后的词，把代价压到 0 也**根本进不了候选池**。所以边收集改成"扫完整组、按调整后代价挑前 N"，并且 `count > 0` 的词**无条件建边**（组内硬上限 32）。不做这条，用户学习在简拼上等于白做 | `pinyin_engine.c` 的 `pinyin_engine_convert`（词边 / 缩写边那两段） |
| **回溯指针必须和路径表同步搬迁**：`path_push` 会重排和淘汰路径，`s_bp` 漏搬一处不会崩，只是 learn() 回溯到**别人的**分词上 —— 表现为"学习记错词"。它能成立全靠这一条：外层 `for i`、内层只写 `j > i`，所以处理位置 i 时 `s_paths[i]` 已冻结、slot 编号不再变 | `pinyin_engine.c` 的 `path_push` |
| **去重时要挑"词边最多"的那条路径**：同一段文本常有多条路径（词边「你好」vs 字边「你+好」），最便宜的那条若全是字边，回溯出来一个词都没有 —— 表现为"学习时灵时不灵"。代价仍用最小的那条，只有 trace 取词边最多的 | `pinyin_engine.c` 的 `pinyin_engine_convert` 末尾去重 |
| **缩写键的打包槽数不能用"数据里实际最长键"**：那是 `PE_MAX_ABBR_LEN`（可能小于 4），而打包固定用 4 槽。拿它当槽数会让 C 和 Python 的打包结果错位 —— **简拼全部失灵，而且不报任何错**。C 侧用独立的 `PE_ABBR_SLOTS`、脚本侧用 `ABBR_SLOTS`，两边都有断言守着 | `pinyin_engine.c` 的 `PE_ABBR_SLOTS`；`gen_engine_dict.py` 的 `ABBR_SLOTS` |
| **自检会写用户表，必须存档还原**：`selftest` 里那段"用户学习"验收会往用户表里塞词，测完要还原，否则**每次开机自检都把用户真实的使用习惯冲掉**。也因此它必须**晚于 `pinyin_learn_init()`** 调用 | `pinyin_engine/selftest.c` 的 `selftest_learn`；`main/main.c` 的调用顺序 |
| **学习粒度必须是"词"而不是"输入串"**：用户打 `nh` 想要「你好」，而它在候选栏里**根本不存在**（缩写组内排第 9，取不到）—— 他选不到，就永远教不会系统。"输入串 -> 文本"那种整串记忆在这里是死路：它只能记住**本来就能选到**的东西 | `components/pinyin_engine/include/pinyin_engine.h` 的"用户学习"一节 |
| **拼音写进输入框 = 引入了"记账"问题**：上屏从"追加"变成了"先把末尾那串拼音删掉、再追加汉字"，而四种情况会让 `s_py_len` 这笔账失真（**App 清空过输入框**、用户中途插数字、光标被挪走、退格分两档）。做法是**不维护计数器、每次动手前核对**：`ta_tail_is_pinyin()` 比一下"末尾这几个字节是不是就是 `s_py`"，对不上就当账已过期、只清缓冲不回删。比猜稳得多 | `components/pinyin_keyboard/pinyin_keyboard.c` 的 `ta_tail_is_pinyin` |
| **`lv_keyboard` 的布局表外部改不了**：`default_kb_map_lc/uc/spec` 和各自的 `ctrl_map` 都是 `lv_keyboard.c` 里的**文件内 static**；`lv_keyboard_get_map_array()` 只能读回 map，**ctrl_map 连读都读不到**（只有 `set`）。所以"想在键面上写个『中』字"做不到 —— 要么整份复制它那套表，要么自己用 `lv_buttonmatrix` 搭。本项目选了后者 | `components/pinyin_keyboard/` 顶部 |
| **`lv_buttonmatrix_set_map()` 会把 ctrl_map 冲掉**：换布局表会重建按键区域，宽度和 `CHECKED`（灰色底）一起丢。所以**"换 map"和"贴 ctrl_map"必须永远成对做** —— 本项目走 `apply_page()` 一处收口。⚠️ 只在换页时记得、却漏了**改键面文字**那条路（切中/英要把 `中` 换成 `EN`，那也是重设 map）就会踩到：症状是**一切语言，所有键宽退回默认、灰键变白** | `components/pinyin_keyboard/pinyin_keyboard.c` 的 `apply_page` |
| **布局表里的 `"\n"` 不是按键，但 `"\n"` 之后的按键下标不跳号**：`lv_buttonmatrix_get_button_text()` 内部会自己跳过 `"\n"`，而 `get_selected_button()` 返回的就是**不含 `"\n"`** 的按键下标。所以按键语义的对照表必须按"真按键"建一份，不能直接拿布局表下标去索引 —— 差一个就会认错键（而且只有少数键会错，很难看出来） | `components/pinyin_keyboard/pinyin_keyboard.c` 的 `build_page` |
| **主题对 `lv_buttonmatrix` 和 `lv_keyboard` 是两套样式**：buttonmatrix 拿到的是"卡片"（圆角+边框），keyboard 拿到的是"屏底色+小内边距"**外加**两条键样式（`bg_color_white`、`keyboard_button_bg`）。所以自己搭的键盘**外观必须显式设**，不能指望主题给成一样的 | `components/pinyin_keyboard/pinyin_keyboard.c` 的 `apply_style` |

## 与 demo1 的关系

demo1 有的、这里已覆盖：桌面、app_manager、相机（+ 拍照存卡）、相册、屏幕/触摸、SD 卡。

这里多出来的：`kv_store`、`time_service`、`net_time`（SNTP 对时）、`wifi_service`、`weather_service`（HTTPS + JSON）、全局状态栏、`picture` 图片资源组件、
**一整套中文输入**（`font_cjk` 中文字体 + `pinyin_engine` 整句/简拼转换引擎 +
`pinyin_keyboard` 自研键盘 + `pinyin_learn` 用户学习持久化）、
Camera 的**录像能力**（`avi_writer` + 常驻录像任务）与相册的**视频播放**（`avi_reader`
+ 同步播放定时器）、Clock / Demo / Settings 三个 App，
以及**每个 App 的 enter/leave 生命周期与资源回收纪律**
（demo1 的 App 是 `create_screen/destroy_screen`，没有回收约定）。

demo1 里有、这里**还没做**的：画板 App（`demo1/app/app_paint.c`）。
移植时唯一要小心的是画布缓冲区（PSRAM）的释放时机 —— 这里删 screen 是异步的，
buffer 不能紧跟其后就 free，得挂到对象的 `LV_EVENT_DELETE` 回调里。
