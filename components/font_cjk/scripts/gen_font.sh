#!/usr/bin/env bash
#
# 生成中文字体 fonts/lv_font_cjk_18.c（LVGL 位图字体，纯 rodata）。
#
# 用法：
#   bash components/font_cjk/scripts/gen_font.sh
#
# 依赖：
#   - lv_font_conv（npm 全局包）—— 组件管理器不会装它，需自行 `npm i -g lv_font_conv`
#   - 思源黑体 SC Normal 的 OTF。它在 lvgl 托管组件里，**首次构建之后**才存在：
#       managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf
#     （所以脚本必须在至少编译过一次之后再跑；换机器同理）
#     也可以用环境变量覆盖：OTF=/path/to/font.otf bash gen_font.sh
#
# ── 为什么是这些参数 ──────────────────────────────────────────────
#   --size 18   与项目全局默认字体（CONFIG_LV_FONT_DEFAULT_MONTSERRAT_18）同号，
#               中英混排时两边字号一致。
#   --bpp 4     抗锯齿，4bpp 是 LVGL 中文字体的常规选择。
#   **不加 --no-compress（即用默认的 RLE 压缩）** —— 压缩版 967 KB、不压缩 1036 KB
#               （本字表 @18px），压缩反而更小。但注意：**压缩字体需要 LVGL 开启
#               `CONFIG_LV_USE_FONT_COMPRESSED`**，否则 lv_font_get_bitmap_fmt_txt()
#               会直接 return NULL —— 表现为"中文一片空白，而且因为没有缺字形所以
#               连占位方块都不画"，极难排查。本工程的 sdkconfig.defaults 里已开。
#   --symbols   用"真字表"而不是连续区段：汉字在 Unicode 里是散落的。
#
# ── 关于 1 MB 位图上限 ──────────────────────────────────────────
#   LVGL 的 glyph_dsc.bitmap_index 默认只有 20 位，即位图最大 1 MB
#   （src/font/fmt_txt/lv_font_fmt_txt.h）。超了 lv_font_conv 会在生成的 .c 里埋
#   `#error "Too large font or glyphs"`。
#   **本工程的 sdkconfig.defaults 已开 CONFIG_LV_FONT_FMT_TXT_LARGE=y**，索引变成
#   32 位，这个上限就没有了。所以下面仍打印"占 1MB 的比例"只是当个体检指标看，
#   不再是硬约束。真换更大的字表/字号也不需要动配置。
#
# ── 字表出处（chars/guifan_8105.txt）────────────────────────────
#   《通用规范汉字表》全表 8105 字（一级 3500 + 二级 3000 + 三级 1605）。
#   取自 GitHub iDvel/The-Table-of-General-Standard-Chinese-Characters（Rime 字表），
#   已与教育部系统的官方 PDF 全文逐字交叉验证：
#     - 一级字表 3500 字与官方 PDF **顺序完全一致**
#     - 去重后恰为 8105 字（与官方数字吻合）
#     - 修正 1 处：官方 PDF 该位是「峏」、全篇无「耑」，仓库那份写成「耑」，已改
#   注：其中 196 字在 CJK 扩展 B（补充平面 U+20000+），77 字在扩展 A。
#   思源黑体 SC 对全部 8105 字都有字形（已核对，缺 0 个）。
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMP="$(cd "$HERE/.." && pwd)"              # components/font_cjk
ROOT="$(cd "$COMP/../.." && pwd)"           # 工程根

CHARS_FILE="$COMP/chars/guifan_8105.txt"
OUT="$COMP/fonts/lv_font_cjk_18.c"
OTF="${OTF:-$ROOT/managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf}"

[ -f "$CHARS_FILE" ] || { echo "找不到字表: $CHARS_FILE" >&2; exit 1; }
[ -f "$OTF" ] || {
    echo "找不到字体文件: $OTF" >&2
    echo "（它在 lvgl 托管组件里，需要先成功构建过一次；或用 OTF=... 指定）" >&2
    exit 1
}
command -v lv_font_conv >/dev/null || { echo "找不到 lv_font_conv（npm i -g lv_font_conv）" >&2; exit 1; }

CHARS="$(python -c "import pathlib,sys;sys.stdout.write(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8'))" "$CHARS_FILE")"
echo "字表: $(python -c "import sys;print(len(sys.argv[1]))" "$CHARS") 字"

# 标点/符号"全家桶"。这些是"动态内容里会出现"的符号，不是可选项：
#   ASCII(95) + Latin-1 补充(96)  —— 基础拉丁、数字、° ± µ × ÷
#   通用标点(0x2000-0x206F)       —— – — … ' ' " " • † ‰
#   货币(0x20A0-0x20CF)           —— € ¥ ₩
#   箭头(0x2190-0x21FF) / 数学(0x2200-0x22FF) / 几何方块(0x25A0-0x25FF)
#   CJK 符号标点(0x3000-0x303F)   —— 、。〈〉《》「」『』【】〔〕
#   全角 ASCII(0xFF00-0xFFEF)     —— ，．！？：；（）ＡＢ１２
# 合计约 657 个码位，@18px 只占约 40 KB。
RANGES=(
    -r 0x20-0x7F
    -r 0xA0-0xFF
    -r 0x2000-0x206F
    -r 0x20A0-0x20CF
    -r 0x2190-0x21FF
    -r 0x2200-0x22FF
    -r 0x25A0-0x25FF
    -r 0x3000-0x303F
    -r 0xFF00-0xFFEF
)

echo "生成中: $OUT"
lv_font_conv --font "$OTF" --size 18 --bpp 4 --format lvgl \
    "${RANGES[@]}" --symbols "$CHARS" \
    -o "$OUT" --force-fast-kern-format

# 自查：生成器若认为字体过大，会埋一句 #error。本工程已开 LV_FONT_FMT_TXT_LARGE，
# 所以这里只提醒（真报了也不再是问题，但说明该复核一下 sdkconfig）。
if grep -q "Too large font" "$OUT"; then
    echo "⚠️ 生成物带了 'Too large font' 的 #error —— 需要 CONFIG_LV_FONT_FMT_TXT_LARGE=y（本工程已开）" >&2
fi

# 统计位图字节数。⚠️ 注意不能用 `grep -o '0x[0-9a-fA-F]\{2\}'` 数：字节 < 0x10 会被
# 写成 1 位十六进制（如 `0xe`），那样会漏掉一大批（不压缩数据里尤其多，实测差近一倍）。
python - "$OUT" <<'PY'
import pathlib, re, sys
p = pathlib.Path(sys.argv[1])
s = p.read_text(encoding='utf-8', errors='ignore')
body = re.sub(r'/\*.*?\*/', '', re.search(r'glyph_bitmap\[\] = \{(.*?)\n\};', s, re.S).group(1), flags=re.S)
n = len(re.findall(r'\b0x[0-9a-fA-F]{1,2}\b', body))
fmt = re.search(r'\.bitmap_format = (\d+)', s).group(1)
print("✅ 完成: %s" % p)
print("   源码 %.1f MB, 字形位图 %d KB（bitmap_format=%s，1=压缩）"
      % (p.stat().st_size / 1048576, n / 1024, fmt))
PY
