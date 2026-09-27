/*
 * font_cjk —— 把中文字体接成"全局默认"。
 *
 * ===================== 字体资源从哪来 =====================
 *
 * fonts/lv_font_cjk_18.c  是 lv_font_conv 生成的纯数据（约 7.1 MB 源码、
 *                         字形位图 967 KB），**永不手改**。要换字表/字号就重跑：
 *
 *     bash components/font_cjk/scripts/gen_font.sh
 *
 *   字体：思源黑体 SC Normal（Source Han Sans SC），SIL OFL 1.1，可商用。
 *         OTF 由 lvgl 托管组件带着（managed_components/lvgl__lvgl/scripts/
 *         built_in_font/SourceHanSansSC-Normal.otf），所以脚本要在成功构建
 *         过一次之后再跑。
 *   字表：chars/guifan_8105.txt —— 《通用规范汉字表》全表 8105 字
 *         （一级 3500 + 二级 3000 + 三级 1605）。取自 GitHub
 *         iDvel/The-Table-of-General-Standard-Chinese-Characters，
 *         并与教育部系统的官方 PDF 全文逐字交叉验证过：
 *           - 一级字表 3500 字与官方 PDF **顺序完全一致**
 *           - 去重后恰为 8105 字（与官方公布的数字吻合）
 *           - 修正 1 处：官方 PDF 该位是「峏」、全篇无「耑」，仓库那份写成「耑」
 *         其中 196 字在 CJK 扩展 B（补充平面 U+20000+），77 字在扩展 A；
 *         思源黑体 SC 对全部 8105 字都有字形（核对过，缺 0 个）。
 *         另外还打进了约 657 个标点/符号码位（ASCII、Latin-1 补充、通用标点、
 *         箭头/数学/几何、CJK 符号标点、全角 ASCII）。
 *
 * ===================== 依赖两个 sdkconfig 开关（都在 defaults 里已开）=====================
 *
 * 这是在 demo2 上唯一需要动 LVGL 配置的地方，两个都是**必需**的：
 *
 * 1) CONFIG_LV_USE_FONT_COMPRESSED
 *    lv_font_conv 默认输出 RLE **压缩**格式的位图（生成物里 .bitmap_format = 1）。
 *    LVGL 的压缩解码器默认关闭（Kconfig 里那个 bool 没有 default）。不开的话
 *    lv_font_get_bitmap_fmt_txt() 会直接 return NULL（即 src/font/fmt_txt/
 *    lv_font_fmt_txt.c 里那条"未开启 LV_USE_FONT_COMPRESSED"的 #else 分支）。
 *    **症状极隐蔽：中文一片空白，而且连占位方块都没有** —— 因为 LV_USE_FONT_PLACEHOLDER
 *    只管"找不到字形"，而我们这里是"找到了字形、但取不到位图"，走的是另一条路。
 *
 * 2) CONFIG_LV_FONT_FMT_TXT_LARGE
 *    把 glyph_dsc.bitmap_index 从 20 位放宽到 32 位。20 位即位图上限 1 MB
 *    （src/font/fmt_txt/lv_font_fmt_txt.h），超了 lv_font_conv 会在生成的 .c 里埋
 *    `#error "Too large font or glyphs"`。当前 967 KB（94.5%）虽未撞线，但换字号/
 *    换更大字表立刻会超，所以直接开掉，省得以后反复动配置。
 *    代价：glyph_dsc 每字形 8→12 字节，本字体 8763 字形约 +34 KB。
 *
 * ⚠️ 为什么不用 --no-compress 绕开第 1 项：本字表 @18px 不压缩是 1036 KB
 *    （**超过 1 MB，反而还得靠第 2 项兜底**），压缩是 967 KB —— 压缩更小，
 *    所以留着压缩、把解码器开起来才是最优组合。
 *
 * ===================== 为什么用 fallback 而不是直接替换 =====================
 *
 * 直接把默认字体换成思源黑体也行，但 apps/weather.c 里显式用了
 * montserrat_20/48/24/16/22/14 —— 那样界面上会**同时出现两种拉丁字体**。
 * 用 fallback 则：英文/数字仍由 Montserrat 渲染（观感完全不变），只有
 * Montserrat 没有的字形（中文）才回退到思源黑体。
 * 回退链路见 src/font/lv_font.c:109-123（在 fallback 链上逐个问 get_glyph_dsc）。
 *
 * ===================== ⚠️ 必须用 lv_obj_set_style_* 而不是 lv_obj_add_style =====================
 *
 * 这是本项目踩过的坑，务必别改回去。
 *
 * 根对象的字体是**主题**设的：lv_theme_default 的 theme_apply() 里对根对象调
 * `lv_obj_add_style(obj, &theme->styles.scr, 0)`。如果我们也在根对象上
 * `lv_obj_add_style()` 加一条字体样式，**不会生效**：
 *
 *   - 对象内部的样式数组布局是 `[transition…][local…][normal…]`
 *     （src/core/lv_obj_style.c:123-131），取值时 `get_prop_core()` **正序遍历、
 *     同一 state 命中即返回**（:830-868）—— 也就是**数组靠前的优先级高**。
 *   - `lv_obj_add_style()` 加的是 **normal** 样式，会被追加在主题样式**后面**
 *     → 索引更大 → 主题的 styles.scr 先被命中 → 我们的字体被无视。
 *   - `lv_obj_set_style_text_font()` 走 `lv_obj_set_local_style_prop()`
 *     （src/core/lv_obj_style_gen.c:646-652）→ 变成 **local** 样式，
 *     排在 normal 样式**前面** → 优先级高于主题 ✔
 *
 * 所以下面一律用 lv_obj_set_style_text_font()。同理，App 里要覆盖主题给的字号，
 * 也应该用 lv_obj_set_style_* 而不是自己 lv_obj_add_style()。
 *
 * ===================== 为什么不用动 line_height / base_line =====================
 *
 * 两个字体纵向度量不同，很容易担心混排会把中文裁掉，所以这里记下实测数据：
 *
 *     Montserrat 18 行盒：基线上方 17px / 下方 4px   （line_height 21, base_line 4）
 *     思源黑体 18 的全部汉字：基线上方 16px / 下方 2px
 *     会用到中文标点（、。〈〉《》「」『』【】〔〕〖〗）：最大 16 / 3
 *
 * 汉字和常用标点都装得进 Montserrat 的行盒，**所以不需要覆盖 line_height** ——
 * 英文的排版因此一点没变。整个字表里只有 3 个永远不会用到的字形会超出：
 * 〱〲（假名竖排重复符，24/10）和 〫〬（组合声调符，19/-16）。
 *
 * 纵向定位公式（想复核的话）：glyph 顶边距行顶 = (line_height - base_line)
 * - box_h - ofs_y，见 src/font/fmt_txt/lv_font_fmt_txt.c:626。
 *
 * ===================== 已知限制 =====================
 *
 * 中文只做了 18px 一档（fallback 是"每个字体对象一份"，而字号是按字体对象定的）。
 * 所以显式设成别的字号的 label（apps/weather.c 的 48/24/20/16/22/14）里的中文
 * 仍然是方块。要在那些地方显示中文，就得为对应字号也生成一份 CJK 字体。
 */
#include "font_cjk.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"      /* lvgl_port_lock：从非 LVGL 任务调 LVGL 必须加锁 */
#include "lvgl.h"

static const char *TAG = "font_cjk";

/* 生成物里的字体对象（fonts/lv_font_cjk_18.c 定义为非 static，可直接 extern） */
extern const lv_font_t lv_font_cjk_18;

/*
 * 混排字体：以项目当前的默认字体为"门面"，中文靠 fallback 落到思源黑体。
 *
 * ⚠️ 必须是**可变副本**：LVGL 自带的字体都是 const，就地改不了 fallback 字段。
 *    用 LV_FONT_DEFAULT 而不是写死 montserrat_18 —— 这样将来改默认字体档位时，
 *    这里的"英文部分"会自动跟着变。
 */
static lv_font_t s_mixed;

/* 给一个"根对象"（screen / layer）装上混排字体。 */
static void apply_mixed_font(lv_obj_t *obj)
{
    /* 必须用 lv_obj_set_style_text_font：它产生 local 样式，优先级高于主题
     * 用 lv_obj_add_style 加的那条 —— 理由见文件顶部"⚠️"那一节。 */
    lv_obj_set_style_text_font(obj, &s_mixed, 0);
}

/*
 * 主题 apply 回调：只在"根对象"上加我们的字体。
 *
 * 为什么只对根对象：lv_theme_default 自带的 theme_apply() 也是这么做的 ——
 * 它只对 parent == NULL 的对象加 theme->styles.scr（字体就在那个样式里），
 * 子对象靠继承（text_font 是 INHERITABLE，见 src/misc/lv_style.c:122；
 * 往上找父对象见 src/core/lv_obj_style.c:1214-1230）。
 */
static void root_font_apply(lv_theme_t *th, lv_obj_t *obj)
{
    LV_UNUSED(th);
    if (lv_obj_get_parent(obj) == NULL) {
        apply_mixed_font(obj);
    }
}

esp_err_t font_cjk_install(void)
{
    lv_display_t *disp = lv_display_get_default();
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "还没有 display —— 必须在 ui_init() 之后再调");

    /* 本函数从 app_main（main 任务）调用，而 LVGL 由它自己的任务在跑 ——
     * 不持锁就是竞态。lvgl_port_lock 是递归锁，从任何上下文调都不会死锁
     * （components/ui/ui_status_bar.c 也是同样的写法）。 */
    lvgl_port_lock(0);

    /* ---- 1) 装配混排字体 ---- */
    s_mixed = *LV_FONT_DEFAULT;             /* 结构体拷贝（英文部分保持项目原样） */
    s_mixed.fallback = &lv_font_cjk_18;     /* 缺字形时逐级回退，中文从这里来 */

    /* ---- 2) 装主题链：让**将来**新建的根对象自动带上它 ----
     *
     * lv_theme_create() 是零初始化（lv_theme.c:47-52），所以只需设 parent + apply_cb。
     * 用 lv_theme_get_from_obj() 取现主题，而不是 lv_theme_default_get() ——
     * 后者在 lv_theme_default.h 里，而那个头文件不在 lvgl.h 的包含链上。
     * 应用顺序由 apply_theme_recursion() 保证：先父主题，再我们的（lv_theme.c:74）。 */
    lv_theme_t *base = lv_theme_get_from_obj(lv_layer_top());
    lv_theme_t *mine = lv_theme_create();
    if (mine == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lv_theme_create 失败");
        return ESP_ERR_NO_MEM;
    }
    lv_theme_set_parent(mine, base);
    lv_theme_set_apply_cb(mine, root_font_apply);
    lv_display_set_theme(disp, mine);

    /* ---- 3) 补**已经存在**的根对象 ----
     *
     * lv_layer_top() / lv_layer_bottom() 在 lv_init() 阶段就建好了，它们的主题
     * 是在那**之前**应用的；而 lv_display_set_theme() 只在一种很窄的条件下才重刷
     * 已有 screen（src/display/lv_display.c:1046-1058），所以吃不到第 2 步的新主题。
     * 而状态栏正好挂在 lv_layer_top() 上（components/ui/ui_status_bar.c:77），
     * 不补这一步状态栏中文就是方块。当前 screen 也一并补上。 */
    lv_obj_t *existing[] = { lv_layer_top(), lv_layer_bottom(), lv_screen_active() };
    for (size_t i = 0; i < sizeof(existing) / sizeof(existing[0]); i++) {
        if (existing[i] != NULL) {
            apply_mixed_font(existing[i]);
        }
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "中文字体已接入（英文走默认字体，中文走 fallback 到思源黑体，规范汉字 8105 字）");
    return ESP_OK;
}
