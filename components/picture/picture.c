/*
 * 图片资源 —— 实现
 *
 * 整个组件就这一个函数：把生成文件里的那个描述符转手交出去。
 *
 * ===================== 图片是怎么来的 =====================
 *
 * 源图：C:\Users\ChenShuang\Desktop\esp32-p4\原始图片\壁纸.png
 *       （1024x600，8-bit RGB，无 alpha 通道）
 *       ⚠️ 源图在**工程目录之外**，所以 images/ 下那份生成的 .c 就是唯一成品，别删。
 *
 * 工具：LVGL 组件自带的 scripts/LVGLImage.py（不用联网、不用装 npm）
 *       它需要 pypng + lz4 —— 系统 Python(C:\Python310) 里两个都有，
 *       IDF 的那个 venv 里没有，所以别用 venv 的 python 跑它。
 *
 * 重新生成（在工程根目录执行，一条命令）：
 *   python managed_components/lvgl__lvgl/scripts/LVGLImage.py --cf RGB565 --ofmt C --name picture_wallpaper_data -o components/picture/images "C:/Users/ChenShuang/Desktop/esp32-p4/原始图片/壁纸.png"
 *
 * 图标的源图和转换在下面"图标"那一段。
 *
 * 两个参数为什么这么选：
 *   --cf RGB565  原图没有 alpha，而屏幕就是 16 位色深；选 RGB565 可以和屏幕格式
 *                一致，LVGL 直接 blit，**不需要任何解码器**。用 RGB565A8 会白扔 600KB。
 *                也考虑过 --compress LZ4，但照片本来就压不动，而且项目没开 LV_USE_LZ4。
 *   --ofmt C     产出的是一个 6.1MB 的 .c 源文件（像素数据本身只有 1.17MB，
 *                剩下全是"0x12,"这种文本表示）。换来的是**转换器把
 *                lv_image_dsc_t 的 6 个字段、对齐、表头都替你填好了**，代码侧零手写。
 *                代价是全量构建时这个文件要重编，多花几秒 —— 已知且可接受。
 *
 * 注意：生成文件的符号名由 --name 决定，必须带上一级前缀（这里是 picture_wallpaper_data），
 *       不能直接叫 wallpaper —— 那会往全局符号表里塞一个太泛的名字。
 *
 * ===================== 图标是怎么来的 =====================
 *
 * 源图：C:\Users\ChenShuang\Desktop\esp32-p4\原始图片\clock.png    (300x300, 无水印)
 *       C:\Users\ChenShuang\Desktop\esp32-p4\原始图片\demo.webp    (800x800)
 *       C:\Users\ChenShuang\Desktop\esp32-p4\原始图片\camera.webp  (800x800)
 *
 * 先经过一步"规范尺寸"处理（裁到内容边界 + 把外圈抠成透明）：
 *   cd 原始图片 && python make_icons_square.py
 *   -> 产出 icons_square/clock.png、demo.png、camera.png（96x96 带 alpha）
 *   -> 里面用 Pillow 做两件事：① 按 min(R,G,B) 阈值裁到内容边界，让内容铺满 96px
 *      ② 从四角洪水填充，把"和边角连通的白色"抠成透明（图形内部的白色不受影响）
 *   ⚠️ 那两个坑都写在脚本注释里了：源图带水印时阈值法会失效（要改最大连通块）；
 *      Pillow 的 floodfill 阈值是"各通道差之和"，不是单通道差。
 *   ⚠️ 脚本会打印"裁了多少 px（占原图宽度的百分之几）"，这个数字要顺手看一眼：
 *      clock 62%、demo 85% 是正常的；camera 只有 37%，因为它是一台横向的相机、
 *      本来就只占画布中间一小块（阈值切过头的表现是边缘被削平，不是"占比小"）。
 *
 * 再转成 LVGL 格式（源图带 alpha，所以必须用 RGB565A8 才存得下"透明"）：
 *   python managed_components/lvgl__lvgl/scripts/LVGLImage.py --cf RGB565A8 --ofmt C --name picture_icon_clock_data -o components/picture/images <icons_square/clock.png>
 *   python managed_components/lvgl__lvgl/scripts/LVGLImage.py --cf RGB565A8 --ofmt C --name picture_icon_demo_data  -o components/picture/images <icons_square/demo.png>
 *   python managed_components/lvgl__lvgl/scripts/LVGLImage.py --cf RGB565A8 --ofmt C --name picture_icon_camera_data -o components/picture/images <icons_square/camera.png>
 *   96x96 RGB565A8 = 96*96*3 = 27648 字节/个（2 字节颜色 + 1 字节 alpha）。
 *
 * ==========================================================
 */

#include <string.h>

#include "picture.h"

/*
 * 图片数据在 images/ 下，由 LVGLImage.py 生成，那里的符号就叫这个名字。
 *
 * ⚠️ 变量名 picture_wallpaper_data 和函数 picture_wallpaper() 是**刻意错开**的：
 *    C 里变量和函数在同一个命名空间，如果生成时 --name 写成 picture_wallpaper，
 *    这里就会和函数重名、直接编译报错。
 */
extern const lv_image_dsc_t picture_wallpaper_data;

const lv_image_dsc_t *picture_wallpaper(void)
{
    return &picture_wallpaper_data;
}

/* ===================== 图标 ===================== */

/* 图标数据同样在 images/ 下，由 LVGLImage.py 生成 */
extern const lv_image_dsc_t picture_icon_clock_data;
extern const lv_image_dsc_t picture_icon_demo_data;
extern const lv_image_dsc_t picture_icon_camera_data;

/*
 * id -> 图标的对照表。
 *
 * 加一个 App 的图标 = 三处：① picture.h 里加一个 PICTURE_ICON_* 常量；
 *                          ② 这里加一行；③ CMakeLists 的 SRCS 加一行。
 */
static const struct {
    const char           *id;
    const lv_image_dsc_t *dsc;
} S_ICONS[] = {
    { PICTURE_ICON_CLOCK,  &picture_icon_clock_data  },
    { PICTURE_ICON_DEMO,   &picture_icon_demo_data   },
    { PICTURE_ICON_CAMERA, &picture_icon_camera_data },
};

const lv_image_dsc_t *picture_icon(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(S_ICONS) / sizeof(S_ICONS[0]); i++) {
        if (strcmp(S_ICONS[i].id, id) == 0) {
            return S_ICONS[i].dsc;
        }
    }
    return NULL;    /* 查不到 -> 由调用方自己降级（desktop 是"只显示名字"） */
}
